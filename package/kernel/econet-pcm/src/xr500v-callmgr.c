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

#define HOOK_PATH  "/sys/kernel/debug/econet-slic/hook"
#define RING_PATH  "/sys/kernel/debug/econet-slic/ring"
#define LED_PHONE  "/sys/class/leds/green:phone1"	/* PHONE panel LED for the active
							 * line. With slic_ec=2 the line is
							 * SLIC ch2/EC_2 = jack labeled
							 * "phone1" (case labels are inverted
							 * vs the silicon) */
#define SPEEDDIAL  "/root/voip-speeddial"	/* optional: 1 line, a SIP URI */
#define CTRL_IP    "127.0.0.1"
#define CTRL_PORT  4444

enum { IDLE, RINGING, INCALL };
static int state = IDLE;

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

/* Drive the PHONE panel LED to match the OEM manual: 2 = blink (ringing, via the
 * kernel timer trigger), 1 = solid on (off-hook / in a call), 0 = off (on-hook). */
static void phone_led(int mode)
{
	const char *trig = (mode == 2) ? "timer" : "none";
	int fd;

	fd = open(LED_PHONE "/trigger", O_WRONLY);
	if (fd >= 0) {
		if (write(fd, trig, strlen(trig)) < 0)
			;
		close(fd);
	}
	if (mode == 2) {
		fd = open(LED_PHONE "/delay_on", O_WRONLY);
		if (fd >= 0) {
			if (write(fd, "400", 3) < 0)
				;
			close(fd);
		}
		fd = open(LED_PHONE "/delay_off", O_WRONLY);
		if (fd >= 0) {
			if (write(fd, "400", 3) < 0)
				;
			close(fd);
		}
	} else {
		fd = open(LED_PHONE "/brightness", O_WRONLY);
		if (fd >= 0) {
			if (write(fd, mode ? "1" : "0", 1) < 0)
				;
			close(fd);
		}
	}
}

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
static void ctrl_cmd(int s, const char *cmd, const char *params)
{
	char json[256], msg[300];
	int jl, ml;

	if (params && *params)
		jl = snprintf(json, sizeof(json),
			"{\"command\":\"%s\",\"params\":\"%s\",\"token\":\"x\"}",
			cmd, params);
	else
		jl = snprintf(json, sizeof(json),
			"{\"command\":\"%s\",\"token\":\"x\"}", cmd);
	ml = snprintf(msg, sizeof(msg), "%d:%s,", jl, json);
	if (write(s, msg, ml) < 0)
		;
	fprintf(stderr, "[callmgr] -> cmd %s %s\n", cmd, params ? params : "");
	fflush(stderr);
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
	if (n > 0)
		ctrl_cmd(s, "dial", uri);
}

/* Ring cadence (in 150ms ticks): ~0.75 s on, ~1.05 s off. The SLIC ring voltage
 * corrupts the SIGREG hook bit, so we only sample the hook during the OFF gap --
 * a shorter cadence means the gaps come sooner, so answering is more responsive
 * (worst-case answer latency ~= ring-on + debounce ~= 1.2 s). */
#define RING_ON_TICKS	5
#define RING_PERIOD	12
#define DEBOUNCE	3	/* consecutive stable reads before a hook change counts */

int main(void)
{
	char buf[8192];
	int s = -1, blen = 0;
	int hook_last = -1, stable_hk = -1, stable_cnt = 0, baseline = 0;
	int ring_tick = 0, ring_on = 0;	/* ring_on = SLIC ring state we last asserted */

	logmsg("starting");
	phone_led(0);			/* idle: PHONE LED off */
	for (;;) {
		struct pollfd pfd;
		int rc, ringing_now = 0;

		if (s < 0) {
			s = ctrl_connect();
			if (s < 0) { sleep(2); continue; }
			blen = 0;
			logmsg("connected to ctrl_tcp");
		}

		pfd.fd = s;
		pfd.events = POLLIN;
		rc = poll(&pfd, 1, 150);	/* 150ms tick: cadence + hook poll */

		/* ---- baresip events ---- */
		if (rc > 0 && (pfd.revents & POLLIN)) {
			int n = read(s, buf + blen, sizeof(buf) - 1 - blen);
			char *q;

			if (n <= 0) { close(s); s = -1; continue; }
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
				payload = colon + 1;
				if (payload + len + 1 > buf + blen)
					break;		/* incomplete, wait for more */
				payload[len] = 0;	/* terminate the JSON (over the ',') */

				if (strstr(payload, "\"CALL_INCOMING\"")) {
					if (state == IDLE) {
						state = RINGING;
						ring_tick = 0;
						phone_led(2);	/* ringing: blink */
						logmsg("INCOMING -> ringing");
					}
				} else if (strstr(payload, "\"CALL_ESTABLISHED\"")) {
					state = INCALL;
					ring(0);
					phone_led(1);	/* in call: solid on */
					logmsg("ESTABLISHED");
				} else if (strstr(payload, "\"CALL_CLOSED\"")) {
					state = IDLE;
					ring(0);
					phone_led(0);	/* idle: off */
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

		/* ---- hook poll (debounced; skip while the ring is energized) ---- */
		if (!ringing_now) {
			int hk = read_hook();

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
						fprintf(stderr, "[callmgr] hook %d->%d (state=%d)\n",
							hook_last, stable_hk, state);
						fflush(stderr);
						if (stable_hk == 1) {		/* off-hook (lifted) */
							phone_led(1);	/* off-hook: solid on */
							if (state == RINGING) {
								ctrl_cmd(s, "accept", NULL);
								ring(0);
							} else if (state == IDLE) {
								speeddial(s);
							}
						} else {			/* on-hook (hung up) */
							phone_led(0);	/* on-hook: off */
							if (state == INCALL || state == RINGING) {
								ctrl_cmd(s, "hangup", NULL);
								ring(0);
								state = IDLE;
							}
						}
					}
					hook_last = stable_hk;
				}
			}
		}
	}
	return 0;
}
