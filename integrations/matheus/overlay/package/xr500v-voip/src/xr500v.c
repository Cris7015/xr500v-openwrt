/**
 * @file xr500v.c  baresip audio driver for the TP-Link Archer XR500v FXS line
 *
 * Reads the SLIC microphone and writes the earpiece through the kernel char
 * device /dev/xr500v-voice (16-bit signed linear, 8 kHz, mono -- host-native
 * byte order, which on the MIPS BE SoC matches the device's big-endian PCM).
 *
 * The char device is full-duplex behind a single fd: opening it runs the SLIC
 * line-up + starts the PCM streaming engine, closing it stops it. baresip
 * allocates the ausrc and auplay independently, so the fd is opened once and
 * reference-counted, shared by both the record and playback threads.
 */
#define _DEFAULT_SOURCE 1
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <math.h>
#include <re.h>
#include <re_atomic.h>
#include <rem.h>
#include <baresip.h>
#include <speex/speex_echo.h>
#include "xr500v-denoise.h"

#define XR_DEV "/dev/xr500v-voice"

static struct ausrc *ausrc;
static struct auplay *auplay;

/* shared full-duplex fd, reference counted across ausrc + auplay */
static int g_fd = -1;
static int g_ref;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
/* One process/device per FXS line. Opt-in only: baseline stays unchanged. */
#define AEC_FRAME 80
static SpeexEchoState *g_aec;
static bool g_aec_active;
static int playback_percent = 100;

struct noise_stats {
	uint64_t energy;
	int64_t sum;
	uint32_t samples;
};

static void measure_noise(const int16_t *samples, size_t count, struct noise_stats *n)
{
	size_t i;
	for (i = 0; i < count; i++) {
		int32_t x = samples[i];
		n->sum += x;
		n->energy += (uint32_t)(x * x);
	}
	n->samples += count;
}

static void report_noise(const char *stage, struct noise_stats *n)
{
	double mean, power, variance;
	if (!n->samples) return;
	mean = (double)n->sum / n->samples;
	power = (double)n->energy / n->samples;
	variance = power - mean * mean;
	info("xr500v: noise stage=%s samples=%u rms=%u ac_rms=%u dc=%d\n",
	     stage, n->samples, (unsigned)sqrt(power),
	     (unsigned)sqrt(variance > 0 ? variance : 0), (int)mean);
	memset(n, 0, sizeof(*n));
}

/* Level counters only: never record or log speech samples. */
static void measure_level(const int16_t *samples, size_t count,
			  unsigned *peak, unsigned long *near_full)
{
	size_t i;
	for (i = 0; i < count; i++) {
		unsigned magnitude = samples[i] < 0 ? -(int)samples[i] : samples[i];
		if (magnitude > *peak) *peak = magnitude;
		if (magnitude >= 32000) (*near_full)++;
	}
}

static bool aec_requested(void)
{
	const char *env = getenv("XR500V_AEC");
	return env && !strcmp(env, "1");
}

static bool denoise_requested(void)
{
	const char *env = getenv("XR500V_DENOISE");
	return env && !strcmp(env, "1");
}

static void aec_playback(const int16_t *samples, size_t count)
{
	size_t i;
	pthread_mutex_lock(&g_lock);
	if (g_aec && g_aec_active)
		for (i = 0; i + AEC_FRAME <= count; i += AEC_FRAME)
			speex_echo_playback(g_aec, samples + i);
	pthread_mutex_unlock(&g_lock);
}

static void aec_capture(int16_t *samples, size_t count)
{
	int16_t out[AEC_FRAME];
	size_t i;
	pthread_mutex_lock(&g_lock);
	if (g_aec && g_aec_active) {
		for (i = 0; i + AEC_FRAME <= count; i += AEC_FRAME) {
			speex_echo_capture(g_aec, samples + i, out);
			memcpy(samples + i, out, sizeof(out));
		}
	}
	pthread_mutex_unlock(&g_lock);
}

static int xr_get_fd(void)
{
	int fd = -1;
	const char *device = getenv("XR500V_DEVICE");

	if (!device || !*device)
		device = XR_DEV;

	pthread_mutex_lock(&g_lock);
	if (g_ref == 0) {
		g_fd = open(device, O_RDWR);
		if (g_fd < 0) {
			warning("xr500v: open(%s) failed (%m)\n", device, errno);
			goto out;
		}
		if (aec_requested()) {
			int rate = 8000;
			g_aec = speex_echo_state_init(AEC_FRAME, 3200);
			if (!g_aec) {
				close(g_fd); g_fd = -1;
				warning("xr500v: AEC allocation failed\n");
				goto out;
			}
			speex_echo_ctl(g_aec, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
			g_aec_active = false;
			info("xr500v: experimental fixed-point AEC enabled, 10ms frames/400ms tail\n");
		}
	}
	g_ref++;
	fd = g_fd;
 out:
	pthread_mutex_unlock(&g_lock);
	return fd;
}

static void xr_put_fd(void)
{
	pthread_mutex_lock(&g_lock);
	if (g_ref > 0 && --g_ref == 0) {
		if (g_fd >= 0)
			close(g_fd);
		g_fd = -1;
		if (g_aec)
			speex_echo_state_destroy(g_aec);
		g_aec = NULL;
		g_aec_active = false;
	}
	pthread_mutex_unlock(&g_lock);
}

static int read_full(int fd, void *buf, size_t n)
{
	uint8_t *p = buf;
	size_t got = 0;

	while (got < n) {
		ssize_t r = read(fd, p + got, n - got);
		if (r <= 0)
			return -1;
		got += (size_t)r;
	}
	return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	size_t put = 0;

	while (put < n) {
		ssize_t r = write(fd, p + put, n - put);
		if (r <= 0)
			return -1;
		put += (size_t)r;
	}
	return 0;
}

/* ----------------------------------------------------------------- ausrc */

struct ausrc_st {
	SpeexPreprocessState *denoise;
	thrd_t thread;
	RE_ATOMIC bool run;
	int fd;
	int tap_fd;		/* debug: tee captured mic to /tmp/xr500v_captap.raw */
	int16_t *sampv;
	size_t sampc;
	ausrc_read_h *rh;
	void *arg;
	struct ausrc_prm prm;
};

static void ausrc_destructor(void *arg)
{
	struct ausrc_st *st = arg;

	if (re_atomic_rlx(&st->run)) {
		re_atomic_rlx_set(&st->run, false);
		thrd_join(st->thread, NULL);
	}
	if (st->fd >= 0) {
		pthread_mutex_lock(&g_lock);
		g_aec_active = false;
		pthread_mutex_unlock(&g_lock);
		xr_put_fd();
	}
	if (st->tap_fd >= 0)
		close(st->tap_fd);
	if (st->denoise)
		speex_preprocess_state_destroy(st->denoise);
	mem_deref(st->sampv);
}

static int src_thread(void *arg)
{
	struct ausrc_st *st = arg;
	const size_t bytes = st->sampc * sizeof(int16_t);
	uint64_t frames = 0;
	uint64_t next_report = st->prm.srate * 5;
	unsigned mic_peak = 0, clean_peak = 0;
	unsigned long mic_full = 0, clean_full = 0;
	struct noise_stats mic_noise = {0}, clean_noise = {0};
	struct noise_stats denoise_noise = {0};

	while (re_atomic_rlx(&st->run)) {
		struct auframe af;

		if (read_full(st->fd, st->sampv, bytes)) {
			if (re_atomic_rlx(&st->run))
				sys_msleep(5);
			continue;
		}

		measure_level(st->sampv, st->sampc, &mic_peak, &mic_full);
		measure_noise(st->sampv, st->sampc, &mic_noise);
		aec_capture(st->sampv, st->sampc);
		measure_level(st->sampv, st->sampc, &clean_peak, &clean_full);
		measure_noise(st->sampv, st->sampc, &clean_noise);
		if (st->denoise) {
			size_t i;
			for (i = 0; i + AEC_FRAME <= st->sampc; i += AEC_FRAME)
				speex_preprocess_run(st->denoise, st->sampv + i);
			measure_noise(st->sampv, st->sampc, &denoise_noise);
		}
		if (st->tap_fd >= 0)
			(void)write(st->tap_fd, st->sampv,
				    st->sampc * sizeof(int16_t));

		auframe_init(&af, st->prm.fmt, st->sampv, st->sampc,
			     st->prm.srate, st->prm.ch);
		af.timestamp = frames * AUDIO_TIMEBASE / st->prm.srate;
		frames += st->sampc / st->prm.ch;
		if (frames >= next_report) {
			info("xr500v: capture levels mic_peak=%u aec_peak=%u mic_near_full=%lu aec_near_full=%lu\n",
			     mic_peak, clean_peak, mic_full, clean_full);
			report_noise("mic", &mic_noise);
			report_noise("aec", &clean_noise);
			if (st->denoise)
				report_noise("denoise", &denoise_noise);
			mic_peak = clean_peak = 0; mic_full = clean_full = 0;
			next_report = frames + st->prm.srate * 5;
		}

		st->rh(&af, st->arg);
	}
	return 0;
}

static int xr_src_alloc(struct ausrc_st **stp, const struct ausrc *as,
			struct ausrc_prm *prm, const char *device,
			ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ausrc_st *st;
	int err = 0;
	(void)device;
	(void)errh;

	if (!stp || !as || !prm || !rh)
		return EINVAL;
	if ((aec_requested() || denoise_requested()) && (prm->srate != 8000 || prm->ch != 1 ||
	    prm->fmt != AUFMT_S16LE || !prm->ptime || prm->ptime % 10))
		return ENOTSUP;

	st = mem_zalloc(sizeof(*st), ausrc_destructor);
	if (!st)
		return ENOMEM;

	st->prm  = *prm;
	st->rh   = rh;
	st->arg  = arg;
	st->fd   = -1;
	st->tap_fd = -1;
	if (denoise_requested()) {
		st->denoise = xr500v_denoise_create();
		if (!st->denoise) {
			mem_deref(st);
			return ENOMEM;
		}
	}
	st->tap_fd = getenv("XR500V_CAPTAP")
		? open("/tmp/xr500v_captap.raw", O_WRONLY | O_CREAT | O_TRUNC, 0644)
		: -1;
	st->sampc = prm->srate * prm->ch * prm->ptime / 1000;

	st->sampv = mem_alloc(st->sampc * sizeof(int16_t), NULL);
	if (!st->sampv) {
		err = ENOMEM;
		goto out;
	}

	st->fd = xr_get_fd();
	if (st->fd < 0) {
		err = EIO;
		goto out;
	}
	pthread_mutex_lock(&g_lock);
	if (g_aec) {
		speex_echo_state_reset(g_aec);
		g_aec_active = true;
	}
	pthread_mutex_unlock(&g_lock);

	re_atomic_rlx_set(&st->run, true);
	err = thread_create_name(&st->thread, "xr500v_src", src_thread, st);
	if (err) {
		re_atomic_rlx_set(&st->run, false);
		goto out;
	}

	info("xr500v: recording started (%u Hz, %u ch, ptime=%u, denoise=%s)\n",
	     prm->srate, prm->ch, prm->ptime, st->denoise ? "mild-6dB" : "off");
 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;
	return err;
}

/* ---------------------------------------------------------------- auplay */

struct auplay_st {
	thrd_t thread;
	RE_ATOMIC bool run;
	int fd;
	int16_t *sampv;
	size_t sampc;
	auplay_write_h *wh;
	void *arg;
	struct auplay_prm prm;
};

static void auplay_destructor(void *arg)
{
	struct auplay_st *st = arg;

	if (re_atomic_rlx(&st->run)) {
		re_atomic_rlx_set(&st->run, false);
		thrd_join(st->thread, NULL);
	}
	if (st->fd >= 0)
		xr_put_fd();
	mem_deref(st->sampv);
}

static int play_thread(void *arg)
{
	struct auplay_st *st = arg;
	const size_t bytes = st->sampc * sizeof(int16_t);
	struct auframe af;
	uint64_t samples = 0;
	unsigned input_peak = 0, output_peak = 0;
	unsigned long input_full = 0, output_full = 0;
	struct noise_stats output_noise = {0};

	auframe_init(&af, st->prm.fmt, st->sampv, st->sampc,
		     st->prm.srate, st->prm.ch);

	while (re_atomic_rlx(&st->run)) {
		size_t i;
		st->wh(&af, st->arg);
		measure_level(st->sampv, st->sampc, &input_peak, &input_full);
		if (playback_percent != 100)
			for (i = 0; i < st->sampc; i++)
				st->sampv[i] = (int32_t)st->sampv[i] * playback_percent / 100;
		measure_level(st->sampv, st->sampc, &output_peak, &output_full);
		measure_noise(st->sampv, st->sampc, &output_noise);
		/* Reference is the outgoing PCM queued below, not received RTP.
		 * The echo filter must cover kernel FIFO + DMA playback latency. */
		aec_playback(st->sampv, st->sampc);

		if (write_full(st->fd, st->sampv, bytes)) {
			if (re_atomic_rlx(&st->run))
				sys_msleep(5);
		}
		samples += st->sampc;
		if (samples >= st->prm.srate * st->prm.ch * 5) {
			info("xr500v: playback levels percent=%d input_peak=%u output_peak=%u input_near_full=%lu output_near_full=%lu\n",
			     playback_percent, input_peak, output_peak, input_full, output_full);
			report_noise("speaker", &output_noise);
			samples = 0; input_peak = output_peak = 0;
			input_full = output_full = 0;
		}
	}
	return 0;
}

static int xr_play_alloc(struct auplay_st **stp, const struct auplay *ap,
			 struct auplay_prm *prm, const char *device,
			 auplay_write_h *wh, void *arg)
{
	struct auplay_st *st;
	int err = 0;
	(void)device;

	if (!stp || !ap || !prm || !wh)
		return EINVAL;
	if (aec_requested() && (prm->srate != 8000 || prm->ch != 1 ||
	    prm->fmt != AUFMT_S16LE || !prm->ptime || prm->ptime % 10))
		return ENOTSUP;

	st = mem_zalloc(sizeof(*st), auplay_destructor);
	if (!st)
		return ENOMEM;

	st->prm  = *prm;
	st->wh   = wh;
	st->arg  = arg;
	st->fd   = -1;
	st->sampc = prm->srate * prm->ch * prm->ptime / 1000;

	st->sampv = mem_alloc(st->sampc * sizeof(int16_t), NULL);
	if (!st->sampv) {
		err = ENOMEM;
		goto out;
	}

	st->fd = xr_get_fd();
	if (st->fd < 0) {
		err = EIO;
		goto out;
	}

	re_atomic_rlx_set(&st->run, true);
	err = thread_create_name(&st->thread, "xr500v_play", play_thread, st);
	if (err) {
		re_atomic_rlx_set(&st->run, false);
		goto out;
	}

	info("xr500v: playback started (%u Hz, %u ch, ptime=%u)\n",
	     prm->srate, prm->ch, prm->ptime);
 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;
	return err;
}

/* ---------------------------------------------------------------- module */

static int module_init(void)
{
	int err;
	const char *value = getenv("XR500V_PLAYBACK_PERCENT");

	if (value) {
		char *end;
		long percent = strtol(value, &end, 10);
		if (!*value || *end || percent < 25 || percent > 100)
			return EINVAL;
		playback_percent = percent;
	}
	info("xr500v: playback scale=%d%%, experimental AEC=%s\n",
	     playback_percent, aec_requested() ? "requested" : "off");

	err  = ausrc_register(&ausrc, baresip_ausrcl(), "xr500v", xr_src_alloc);
	err |= auplay_register(&auplay, baresip_auplayl(), "xr500v",
			       xr_play_alloc);
	return err;
}

static int module_close(void)
{
	ausrc  = mem_deref(ausrc);
	auplay = mem_deref(auplay);
	return 0;
}

const struct mod_export DECL_EXPORTS(xr500v) = {
	"xr500v",
	"sound",
	module_init,
	module_close
};
