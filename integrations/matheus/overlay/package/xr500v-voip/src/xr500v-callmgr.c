/*
 * xr500v-callmgr.c -- real call flow for the XR500v FXS, tying the SLIC hook to
 * baresip's ctrl_tcp so the Philips DECT phone behaves like a real telephone:
 *
 *   incoming SIP call            -> ring the handset (SLIC ring, debugfs)
 *   lift handset while ringing   -> accept the call (off-hook)
 *   hang up during a call        -> hangup (line goes idle, no leftover beep)
 *   lift handset while idle      -> speed-dial the URI in /root/voip-speeddial
 *
 * Polls the hook debugfs and speaks baresip's ctrl_tcp netstring/JSON protocol
 * on 127.0.0.1:4444. A single small musl binary (the device only has busybox).
 *
 * Build (OpenWrt toolchain): mips-openwrt-linux-musl-gcc -O2 callmgr.c -o callmgr
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <errno.h>

static char hook_path[128] = "/sys/kernel/debug/econet-slic/hook";
static char ring_path[128] = "/sys/kernel/debug/econet-slic/ring";
static char lpm_path[128] = "/sys/kernel/debug/econet-slic/lpm";
static char audio_setup_path[128] = "/sys/kernel/debug/econet-slic/audio_setup";
static char speeddial_path[128] = "/root/voip-speeddial";
static int isolated_ec;
static int ctrl_port = 4444;
#define HOOK_PATH hook_path
#define RING_PATH ring_path
#define HOOK_RAW   "/sys/kernel/debug/econet-slic/hook_raw"	/* "b0 b1": b0=EC_1=phone2, b1=EC_2=phone1 */
#define SLIC_EC_P  "/sys/module/econet_slic/parameters/slic_ec"	/* active channel (1 or 2) */
#define LPM_PATH lpm_path
#define AUDIO_SETUP_P audio_setup_path
/* PHONE panel LEDs. Case labels are inverted vs the silicon: EC_2 (SLIC ch2) =
 * jack "phone1" = LED_PHONE1; EC_1 (ch1) = jack "phone2" = LED_PHONE2. We ring
 * both jacks so the single phone rings in whichever it is plugged into. */
#define LED_PHONE1 "/sys/class/leds/green:phone1"	/* EC_2 */
#define LED_PHONE2 "/sys/class/leds/green:phone2"	/* EC_1 */
#define SPEEDDIAL speeddial_path
#define CTRL_IP    "127.0.0.1"
#define CTRL_PORT ctrl_port

enum { IDLE, RINGING, INCALL, DIALING, ENDING, COLLECTING };
static int state = IDLE;
static const char *command_token = "x";
static char diagnostic_path[128];
#include "keypad.inc"

static void logmsg(const char *m)
{
	fprintf(stderr, "[callmgr] %s\n", m);
	fflush(stderr);
}

/* hook debugfs prints the bit as ASCII "0"/"1"; return 1=off-hook, 0=on-hook,
 * -1 on error / unreadable. */
static int read_hook(void)
{
	char b[32];
	int fd, n, i;

	fd = open(HOOK_PATH, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	for (i = 0; i < n; i++) {
		if (b[i] == '1')
			return 1;
		if (b[i] == '0')
			return 0;
	}
	return -1;
}

/* Read BOTH channels' hook bits from hook_raw ("b0 b1"): h[0]=EC_1=jack phone2,
 * h[1]=EC_2=jack phone1. Returns 0 on success, -1 on error (caller falls back to
 * the single-channel read_hook). */
static int read_hook_raw(int h[2])
{
	char b[32];
	int fd, n;

	if (isolated_ec) {
		n = read_hook();
		if (n < 0)
			return -1;
		h[0] = h[1] = 0;
		h[isolated_ec - 1] = n;
		return 0;
	}

	fd = open(HOOK_RAW, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	b[n] = 0;
	if (sscanf(b, "%d %d", &h[0], &h[1]) != 2)
		return -1;
	return 0;
}

/* Select the active SLIC channel (1 or 2) the audio/line-up routes to. Written
 * before "accept" so voice_dev_open lines up the jack that was answered. */
static void set_slic_ec(int ec)
{
	char c = (char)('0' + ec);
	int fd;

	if (isolated_ec)
		return;
	fd = open(SLIC_EC_P, O_WRONLY);

	if (fd >= 0) {
		if (write(fd, &c, 1) < 0)
			;
		close(fd);
	}
}

/* ring the SLIC line: 1 = ring (STATE 0x07), 0 = stop (STATE 0x23 active+codec) */
static void ring(int on)
{
	int fd = open(RING_PATH, O_WRONLY);

	if (fd >= 0) {
		if (write(fd, on ? "1\n" : "0\n", 2) < 0)
			;
		close(fd);
	}
}

/* Drive the SLIC line feed on the CURRENT slic_ec channel via the driver's lpm
 * node: standby=1 puts the line in SS_LOWPOWER (silent buck-boost, idle DC-DC
 * coil-whine gone) with the LPM hook comparator armed; standby=0 raises the
 * active feed for a call. */
static void lpm_set(int standby)
{
	int fd = open(LPM_PATH, O_WRONLY);

	if (fd >= 0) {
		if (write(fd, standby ? "1\n" : "0\n", 2) < 0)
			;
		close(fd);
	}
}

/* Idle: park BOTH jacks (EC_1=phone2, EC_2=phone1) in low-power standby so the
 * line is silent in whichever jack the phone sits, and off-hook is still sensed
 * on both via the armed LPM comparator. Leaves slic_ec back on the default (2). */
static void line_standby_both(void)
{
	if (isolated_ec) { lpm_set(1); return; }
	set_slic_ec(1); lpm_set(1);
	set_slic_ec(2); lpm_set(1);
}

/* A channel that goes straight from cold initialization to low-power standby
 * does not reliably arm the Le9642 hook comparator.  PHONE2/EC_1 then remains
 * at hook_raw=0 even after the handset is lifted.  Prime each channel with a
 * short active-feed interval before parking it in standby.  This is needed
 * once at call-manager startup only; normal hang-up paths use
 * line_standby_both() directly and therefore do not pulse the line again. */
static void line_prime_and_standby_both(void)
{
	int ec;

	if (isolated_ec) {
		lpm_set(0); usleep(100000); lpm_set(1);
		return;
	}

	for (ec = 1; ec <= 2; ec++) {
		set_slic_ec(ec);
		lpm_set(0);
		usleep(100000);
		lpm_set(1);
		usleep(100000);
	}
	set_slic_ec(2);
}

/* Off-hook / in-call: raise the active feed on the jack the call is on, and
 * re-run audio_setup to wake the codec/ADC. Standby (STATE 0x0c) powers the
 * codec down, so the FIRST call after boot would otherwise establish with no
 * audio until a second attempt; reading the audio_setup debugfs node re-applies
 * the full audio path (switcher HP + active+codec + slots/gains) every time. */
static void line_active(int ec)
{
	int fd;

	set_slic_ec(ec);
	lpm_set(0);
	fd = open(AUDIO_SETUP_P, O_RDONLY);
	if (fd >= 0) {
		char b[8];
		if (read(fd, b, sizeof(b)) < 0)
			;
		close(fd);
	}
}

/* Drive ONE PHONE panel LED (by sysfs base path): 2 = blink (ringing, via the
 * kernel timer trigger), 1 = solid on (off-hook / in a call), 0 = off (on-hook).
 * Tolerates a missing LED node (open fails -> skipped). */
static void led_set(const char *base, int mode)
{
	const char *trig = (mode == 2) ? "timer" : "none";
	char path[96];
	int fd;

	if (isolated_ec && strcmp(base, isolated_ec == 2 ? LED_PHONE1 : LED_PHONE2))
		return;

	snprintf(path, sizeof(path), "%s/trigger", base);
	fd = open(path, O_WRONLY);
	if (fd >= 0) {
		if (write(fd, trig, strlen(trig)) < 0)
			;
		close(fd);
	}
	if (mode == 2) {
		snprintf(path, sizeof(path), "%s/delay_on", base);
		fd = open(path, O_WRONLY);
		if (fd >= 0) { if (write(fd, "400", 3) < 0) ; close(fd); }
		snprintf(path, sizeof(path), "%s/delay_off", base);
		fd = open(path, O_WRONLY);
		if (fd >= 0) { if (write(fd, "400", 3) < 0) ; close(fd); }
	} else {
		snprintf(path, sizeof(path), "%s/brightness", base);
		fd = open(path, O_WRONLY);
		if (fd >= 0) { if (write(fd, mode ? "1" : "0", 1) < 0) ; close(fd); }
	}
}

/* ringing: blink BOTH jacks (the phone is in one of them). */
static void leds_ring(void) { led_set(LED_PHONE1, 2); led_set(LED_PHONE2, 2); }
/* answered on channel ec: that jack solid on, the other off (ec=2 -> phone1). */
static void leds_answer(int ec)
{
	led_set(LED_PHONE1, ec == 2 ? 1 : 0);
	led_set(LED_PHONE2, ec == 1 ? 1 : 0);
}
static void leds_off(void) { led_set(LED_PHONE1, 0); led_set(LED_PHONE2, 0); }

static int ctrl_connect(void)
{
	struct sockaddr_in a;
	int s = socket(AF_INET, SOCK_STREAM, 0);

	if (s < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(CTRL_PORT);
	inet_aton(CTRL_IP, &a.sin_addr);
	if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(s);
		return -1;
	}
	return s;
}

/* send a baresip ctrl_tcp command as a netstring-framed JSON object */
static int ctrl_cmd(int s, const char *cmd, const char *params)
{
	char json[1200], msg[1240], escaped[800];
	int jl, ml, pos = 0;
	size_t i;
	if (params) {
		if (strlen(params) > 380) return -1;
		for (i = 0; params[i]; i++) {
			unsigned char c = params[i];
			if (c < 32 || c == 127) return -1;
			if (c == '"' || c == '\\') escaped[pos++] = '\\';
			escaped[pos++] = c;
		}
	}
	escaped[pos] = 0;

	if (params && *params)
		jl = snprintf(json, sizeof(json),
			"{\"command\":\"%s\",\"params\":\"%s\",\"token\":\"%s\"}",
			cmd, escaped, command_token);
	else
		jl = snprintf(json, sizeof(json),
			"{\"command\":\"%s\",\"token\":\"%s\"}", cmd, command_token);
	if (jl < 0 || jl >= (int)sizeof(json)) return -1;
	ml = snprintf(msg, sizeof(msg), "%d:%s,", jl, json);
	if (send(s, msg, ml, MSG_NOSIGNAL) != ml) {
		logmsg("control command send failed");
		return -1;
	}
	/* Dial targets may be telephone numbers: never log their contents. */
	fprintf(stderr, "[callmgr] -> cmd %s\n", cmd);
	fflush(stderr);
	return 0;
}

static void speeddial(int s)
{
	char uri[160];
	int fd = open(SPEEDDIAL, O_RDONLY), n;

	if (fd < 0)
		return;
	n = read(fd, uri, sizeof(uri) - 1);
	close(fd);
	if (n <= 0)
		return;
	uri[n] = 0;
	while (n > 0 && (uri[n-1] == '\n' || uri[n-1] == '\r' || uri[n-1] == ' '))
		uri[--n] = 0;
	if (n > 0 && ctrl_cmd(s, "dial", uri) == 0) {
		/* Cancellation must work before CALL_ESTABLISHED arrives. */
		state = DIALING;
		logmsg("OUTGOING -> dialing");
	}
}

static int keypad_finish(int s, int ec, const char *reason)
{
	char uri[380];
	int hook;
	if (!reason) reason = keypad_destination_error();
	if (!reason && keypad_uri(uri, sizeof(uri))) reason = "uri_too_long";
	// Read while the collector still holds active feed. Closing its PCM fd
	// executes slic_dual_release(), which parks the SLIC in 0x0c. A raw hook
	// sample during that transition can falsely report on-hook. Never make
	// the dialing decision from a single post-close comparator sample.
	if (!reason) {
		hook = read_hook();
		if (hook != 1) reason = hook == 0 ? "on_hook_before_handoff" : "hook_unavailable";
	}
	keypad_stop();
	if (!reason) {
		line_active(ec);
		if (!ctrl_cmd(s, "dial", uri)) {
			state = DIALING;
			logmsg("KEYPAD -> dialing (active-feed hook checked before PCM release)");
			return 0;
		}
		reason = "control_send_failed";
	}
	state = IDLE; line_standby_both(); leds_off();
	fprintf(stderr, "[callmgr] keypad cancelled reason=%s digits=%u; hang up to retry\n", reason, digit_count);
	fflush(stderr);
	return -1;
}

/* Ring cadence (in 150ms ticks): ~0.75 s on, ~1.05 s off. The SLIC ring voltage
 * corrupts the SIGREG hook bit, so we only sample the hook during the OFF gap.
 * The 450ms debounce is the silicon settle guard (shortening it -> false hooks).
 * During RINGING we watch BOTH channels' hook bits to learn which jack answered. */
#define RING_ON_TICKS	5
#define RING_PERIOD	12
#define DEBOUNCE	3	/* consecutive stable reads (3*150=450ms) before a hook change counts */

int main(void)
{
	char buf[8192];
	int s = -1, blen = 0;
	int diagnostic_fd = -1, diagnostic_len = 0;
	char diagnostic_buf[256];
	int hook_last = -1, stable_hk = -1, stable_cnt = 0, baseline = 0;
	int ring_tick = 0, ring_on = 0;	/* ring_on = SLIC ring state we last asserted */
	int answered_ec = 2;		/* channel/jack the active call was answered on (default phone1=EC_2) */
	const char *line_env = getenv("XR500V_LINE");
	const char *runtime = getenv("XR500V_RUNTIME_DIR");
	if (!runtime || !*runtime)
		runtime = "/tmp/xr500v-dual";
	if (*runtime != '/' || strlen(runtime) > 80)
		return 2;

	if (line_env) {
		int line = atoi(line_env);

		if (line < 1 || line > 2)
			return 2;
		isolated_ec = 3 - line;
		answered_ec = isolated_ec;
		ctrl_port = 4443 + line;
		snprintf(hook_path, sizeof(hook_path), "/sys/kernel/debug/econet-slic/phone%d/hook", line);
		snprintf(ring_path, sizeof(ring_path), "/sys/kernel/debug/econet-slic/phone%d/ring", line);
		snprintf(lpm_path, sizeof(lpm_path), "/sys/kernel/debug/econet-slic/phone%d/lpm", line);
		snprintf(audio_setup_path, sizeof(audio_setup_path), "/sys/kernel/debug/econet-slic/phone%d/audio_setup", line);
		snprintf(speeddial_path, sizeof(speeddial_path), "%s/phone%d/speeddial", runtime, line);
		snprintf(diagnostic_path, sizeof(diagnostic_path), "%s/phone%d/control", runtime, line);
		snprintf(dialing_path, sizeof(dialing_path), "%s/phone%d/dialing", runtime, line);
		snprintf(keypad_device, sizeof(keypad_device), "/dev/xr500v-voice%s", line == 2 ? "1" : "");
	}
	if (*diagnostic_path) {
		/* ctrl_tcp permits ONE client. Diagnostics must use our connection,
		 * never connect directly and displace the hook/event manager. */
		{
			struct stat st;

			if ((mkfifo(diagnostic_path, 0600) == 0 || errno == EEXIST) &&
			    lstat(diagnostic_path, &st) == 0 && S_ISFIFO(st.st_mode) && st.st_uid == getuid())
				diagnostic_fd = open(diagnostic_path, O_RDWR | O_NONBLOCK);
		}
	}

	logmsg("starting");
	signal(SIGTERM, stop_signal); signal(SIGINT, stop_signal);
	leds_off();			/* idle: both PHONE LEDs off */
	line_prime_and_standby_both();	/* cold boot: arm both hook comparators, then idle */
	while (!stopping) {
		struct pollfd pfd[3];
		int rc, ringing_now = 0;

		if (s < 0) {
			s = ctrl_connect();
			if (s < 0) { sleep(2); continue; }
			blen = 0;
			logmsg("connected to ctrl_tcp");
		}

		pfd[0].fd = s;
		pfd[0].events = POLLIN;
		pfd[1].fd = diagnostic_fd;
		pfd[1].events = POLLIN;
		pfd[2].fd = keypad_fd; pfd[2].events = POLLIN;
		rc = poll(pfd, 3, 150);	/* bounded tick: cadence + hook poll */

		if (rc > 0 && (pfd[1].revents & POLLIN)) {
			int n = read(diagnostic_fd, diagnostic_buf + diagnostic_len,
				     sizeof(diagnostic_buf) - 1 - diagnostic_len);
			if (n > 0) {
				char *end;
				diagnostic_len += n;
				diagnostic_buf[diagnostic_len] = 0;
				while ((end = strchr(diagnostic_buf, '\n'))) {
					char token[48], cmd[24], extra;
					int used = end - diagnostic_buf + 1;
					*end = 0;
					if (sscanf(diagnostic_buf, "%47s %23s %c", token, cmd, &extra) == 2 &&
					    !strncmp(token, "diag-", 5) &&
					    strspn(token, "abcdefghijklmnopqrstuvwxyz0123456789-") == strlen(token) &&
					    (!strcmp(cmd, "mute") || !strcmp(cmd, "listcalls") || !strcmp(cmd, "callstat"))) {
						command_token = token;
						ctrl_cmd(s, cmd, NULL);
						command_token = "x";
					}
					diagnostic_len -= used;
					memmove(diagnostic_buf, diagnostic_buf + used, diagnostic_len);
					diagnostic_buf[diagnostic_len] = 0;
				}
				if (diagnostic_len == sizeof(diagnostic_buf) - 1)
					diagnostic_len = 0;
			}
		}

		/* ---- baresip events ---- */
		if (rc > 0 && (pfd[0].revents & (POLLIN | POLLHUP | POLLERR))) {
			int n = read(s, buf + blen, sizeof(buf) - 1 - blen);
			char *q;

			if (n <= 0) { keypad_stop(); ring(0); leds_off(); line_standby_both(); state = IDLE; baseline = 0; stable_cnt = 0; hook_last = -1; close(s); s = -1; continue; }
			blen += n;
			buf[blen] = 0;
			q = buf;
			for (;;) {		/* parse complete netstrings <len>:<json>, */
				char *colon = memchr(q, ':', blen - (q - buf));
				char *payload;
				int len;

				if (!colon)
					break;
				len = atoi(q);
				if (len < 0 || len > (int)sizeof(buf) - 32) { q = buf + blen; break; }
				payload = colon + 1;
				if (payload + len + 1 > buf + blen)
					break;		/* incomplete, wait for more */
				payload[len] = 0;	/* terminate the JSON (over the ',') */
				if (strstr(payload, "\"token\":\"diag-"))
					logmsg(payload);

				if (strstr(payload, "\"CALL_INCOMING\"")) {
					if (state == COLLECTING) {
						keypad_stop(); ctrl_cmd(s, "hangup", NULL); state = ENDING;
					}
					if (state == IDLE) {
						state = RINGING;
						ring_tick = 0;
						leds_ring();	/* ringing: blink both jacks */
						logmsg("INCOMING -> ringing");
					}
				} else if (strstr(payload, "\"CALL_OUTGOING\"")) {
					if (state == IDLE)
						state = DIALING;
				} else if (strstr(payload, "\"CALL_ESTABLISHED\"")) {
					keypad_stop();
					/* The peer may answer while our cancellation is in flight. */
					if (state == ENDING) {
						ctrl_cmd(s, "hangup", NULL);
						q = payload + len + 1;
						continue;
					}
					state = INCALL;
					ring(0);
					leds_answer(answered_ec);	/* in call: answered jack solid on */
					logmsg("ESTABLISHED");
				} else if (strstr(payload, "\"CALL_CLOSED\"")) {
					keypad_stop();
					state = IDLE;
					ring(0);
					leds_off();	/* idle: both off */
					line_standby_both();	/* back to silent low-power feed */
					answered_ec = isolated_ec ? isolated_ec : 2;
					logmsg("CLOSED -> idle");
				}
				q = payload + len + 1;	/* past json + ',' */
			}
			blen -= (q - buf);
			if (blen > 0)
				memmove(buf, q, blen);
			buf[blen] = 0;
			if (blen > (int)sizeof(buf) - 64)
				blen = 0;	/* runaway guard */
		}

		if (state == COLLECTING) {
			char digits[32];
			int finish = 0, abort = 0, i;
			const char *reason = NULL;
			if (pfd[2].revents & POLLIN) {
				int n = read(keypad_fd, digits, sizeof(digits));
				if (n <= 0) { abort = 1; reason = "collector_read_failed"; }
				for (i = 0; i < n; i++) {
					if (digits[i] == '#') { finish = 1; logmsg("keypad send key detected (#)"); break; }
					if ((digits[i] >= '0' && digits[i] <= '9') || digits[i] == '*') {
						if (digit_count >= sizeof(dial_digits) - 1) { abort = 1; reason = "digit_limit"; break; }
						dial_digits[digit_count++] = digits[i]; dial_digits[digit_count] = 0;
						digit_at = clock_ms();
						logmsg("keypad digit accepted");
						// Local extension labels are not provider phone numbers.
						if (!strcmp(dial_kind, "local") && digit_count == 1 && (digits[i] == '1' || digits[i] == '2')) {
							fprintf(stderr, "[callmgr] local keypad target=Phone%c source=Phone%d\n", digits[i], 3 - isolated_ec);
							fflush(stderr);
						}
					}
				}
			}
			if (pfd[2].revents & (POLLHUP | POLLERR)) { abort = 1; reason = "collector_closed"; }
			if (!finish && digit_count && clock_ms() - digit_at >= 4000) { finish = 1; logmsg("keypad interdigit timeout"); }
			if (clock_ms() - dial_started >= 30000) { abort = 1; reason = "collection_timeout"; }
			if (finish || abort) keypad_finish(s, answered_ec, reason);
		}

		/* ---- ring cadence (only while RINGING) ---- */
		if (state == RINGING) {
			int phase = ring_tick % RING_PERIOD;

			ringing_now = (phase < RING_ON_TICKS);
			/* Only (re)assert the SLIC ring state on a transition.
			 * Writing STATE 0x07 every tick re-enters BAL_RING and
			 * restarts the ring waveform, so the bell stutters / rings
			 * continuously instead of cadencing. Toggle on edges only. */
			if (ringing_now != ring_on) {
				ring(ringing_now);
				ring_on = ringing_now;
			}
			ring_tick++;
		} else {
			if (ring_on) {		/* left RINGING -> make sure ring is off */
				ring(0);
				ring_on = 0;
			}
			ring_tick = 0;
		}

		/* ---- hook poll (debounced; skip while the ring is energized) ----
		 * Waiting for an answer (RINGING/IDLE) we watch BOTH jacks; in a call
		 * we watch only the answered channel (for hang-up). `which` = the jack
		 * that went off-hook, so the audio is routed to it before "accept". */
		if (!ringing_now) {
			int h[2], hk, which = answered_ec;

			if (read_hook_raw(h) == 0) {
				if (state == INCALL || state == DIALING || state == ENDING || state == COLLECTING) {
					hk = h[(answered_ec == 1) ? 0 : 1];
				} else {		/* RINGING or IDLE: either jack */
					hk = h[0] || h[1];
					which = h[0] ? 1 : (h[1] ? 2 : answered_ec);
				}
			} else {
				hk = read_hook();	/* fallback: active channel only */
			}

			if (hk >= 0) {
				if (hk == stable_hk) {
					if (stable_cnt < 50)
						stable_cnt++;
				} else {
					stable_hk = hk;
					stable_cnt = 1;
				}
				if (stable_cnt == DEBOUNCE && stable_hk != hook_last) {
					if (!baseline) {
						baseline = 1;		/* first settle: no action */
					} else {
						fprintf(stderr, "[callmgr] hook %d->%d (state=%d ec=%d)\n",
							hook_last, stable_hk, state, which);
						fflush(stderr);
						if (stable_hk == 1) {		/* off-hook (lifted) */
							if (state == RINGING) {
								answered_ec = which;
								set_slic_ec(answered_ec);	/* route audio to this jack BEFORE accept */
								leds_answer(answered_ec);
								ctrl_cmd(s, "accept", NULL);
								ring(0);
								line_active(answered_ec);	/* wake codec so incoming audio works first try */
							} else if (state == IDLE) {
								answered_ec = which;
								set_slic_ec(answered_ec);
								line_active(answered_ec);	/* raise active feed before dialing */
								leds_answer(answered_ec);
								{
									int collect = keypad_start(s, diagnostic_fd);
									if (collect > 0) { state = COLLECTING; logmsg("KEYPAD -> collecting (30 s maximum)"); }
									else if (!collect) speeddial(s);
									else logmsg("keypad unavailable; hang up to retry");
								}
							} else {
								leds_answer(answered_ec);
							}
						} else {			/* on-hook */
							/* During RINGING the handset is SUPPOSED to be
							 * on-hook (not answered yet), so an on-hook read
							 * here is the NORMAL state and must NOT hang up.
							 * The SLIC SIGREG reads a false off-hook at cold
							 * start (only a BAL_RING cycle corrects it), so the
							 * callmgr's baseline starts at 1; the SIGREG's
							 * 1->0 self-correction during the FIRST ring used
							 * to trip this branch -> spurious hangup -> baresip
							 * 486 Busy Here on the first call. A caller giving
							 * up arrives as CALL_CLOSED, not via the hook, so
							 * INCALL hangs up; DIALING cancels the unanswered call. */
							if (state == INCALL || state == DIALING) {
								ctrl_cmd(s, "hangup", NULL);
								ring(0);
								leds_off();
								line_standby_both();	/* hang-up: back to silent standby */
								/* Do not enqueue another call until this leg closes. */
								state = ENDING;
							} else if (state == IDLE || state == COLLECTING) {
								keypad_stop(); state = IDLE;
								leds_off();
								line_standby_both();	/* lifted then hung up w/o a call: re-park standby */
							}
							/* state == RINGING: keep ringing (bell + LEDs). */
						}
					}
					hook_last = stable_hk;
				}
			}
		}
	}
	keypad_stop(); ring(0); leds_off(); line_standby_both();
	if (s >= 0) close(s);
	if (diagnostic_fd >= 0) close(diagnostic_fd);
	return 0;
}
