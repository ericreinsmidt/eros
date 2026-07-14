#include "btpair.h"

#include <SDL_ttf.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#endif

/* Files shared with launch.sh's reconnect loop. The loop writes the live state
 * ("1"/"0") and backs off its own connect attempts while the pause file exists,
 * so our scan/pair never collides with it on D-Bus. */
#define STATE_F "/tmp/eros_bt_state"
#define PAUSE_F "/tmp/eros_bt_pause"
#define SEEN_F  "/tmp/eros_bt_seen"
#define SCAN_F  "/tmp/eros_bt_scan"
#define PAIR_F  "/tmp/eros_bt_pair"

#define ENGAGE_DELAY  1500  /* ms disconnected before the screen appears (anti-flicker) */
#define GRACE_MAC     8000  /* ms of "Connecting…" for a remembered sink before scanning */
#define GRACE_NODEV   2500  /* ms before scanning when nothing is remembered */

typedef struct { char mac[20]; char name[64]; } bt_entry;
#define BT_MAX 16

enum { PH_CONNECTING, PH_SCAN, PH_LIST, PH_PAIRING, PH_OK, PH_FAIL };

static char        s_cfg[256];
static char        s_mac[32];               /* remembered sink, from eros.cfg */
static TTF_Font   *s_f_title, *s_f_item, *s_f_hint;

static bool        s_want_bt;               /* switch is on BT */
static bool        s_engaged;
static bool        s_dismissed;             /* user backed out; wait for reconnect/toggle */
static int         s_phase;
static Uint32      s_disc_since;            /* first noticed switch-on + disconnected */
static Uint32      s_phase_since;

static bt_entry    s_list[BT_MAX];
static int         s_count, s_cursor;
static pid_t       s_child = -1;
static pid_t       s_probe = -1;            /* fast background link check while the screen waits */
static Uint32      s_probe_at;
#define PROBE_MS 400

#ifdef __linux__
#define SWITCH_DEV "/dev/input/event3"
static int s_sw_fd = -1;

/* current SW_TABLET_MODE bit (evdev doesn't replay it on open) */
static int sw_bit(int fd)
{
	unsigned long bits[SW_MAX / (8 * sizeof(long)) + 1] = { 0 };
	if (ioctl(fd, EVIOCGSW(sizeof bits), bits) < 0) return 0;
	return (bits[SW_TABLET_MODE / (8 * sizeof(long))] >> (SW_TABLET_MODE % (8 * sizeof(long)))) & 1;
}
#endif

/* ---- helpers ---- */

static bool bt_connected(void)
{
	FILE *f = fopen(STATE_F, "r");
	if (!f) return false;
	int v = 0;
	if (fscanf(f, "%d", &v) != 1) v = 0;
	fclose(f);
	return v == 1;
}

static void touch(const char *path)
{
	FILE *f = fopen(path, "w");
	if (f) fclose(f);
}

static void write_state(int connected)
{
	FILE *f = fopen(STATE_F, "w");
	if (f) { fprintf(f, "%d\n", connected ? 1 : 0); fclose(f); }
}

static void child_spawn(const char *cmd)
{
	s_child = fork();
	if (s_child == 0) { execl("/bin/sh", "sh", "-c", cmd, (char *)NULL); _exit(127); }
}

/* rewrite eros.cfg keeping every line except an existing bt_mac=, then append it */
static void save_bt_mac(const char *mac)
{
	char buf[4096];
	size_t n = 0;
	FILE *f = fopen(s_cfg, "r");
	if (f) { n = fread(buf, 1, sizeof buf - 1, f); fclose(f); }
	buf[n] = '\0';
	FILE *o = fopen(s_cfg, "w");
	if (!o) return;
	for (char *p = buf; *p;) {
		char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		if (strncmp(p, "bt_mac=", 7) != 0) fprintf(o, "%.*s\n", (int)len, p);
		if (!nl) break;
		p = nl + 1;
	}
	fprintf(o, "bt_mac=%s\n", mac);
	fclose(o);
	snprintf(s_mac, sizeof s_mac, "%s", mac);
}

/* nameless devices report their address as the name (":" -> "-"); skip those */
static int name_is_addr(const char *mac, const char *name)
{
	char dash[20];
	size_t j = 0;
	for (size_t i = 0; mac[i] && j < sizeof dash - 1; i++) dash[j++] = mac[i] == ':' ? '-' : mac[i];
	dash[j] = '\0';
	return strcasecmp(name, dash) == 0;
}

/* Intersect the names list (`bluetoothctl devices`) with the MACs actually seen
 * in this scan, so bonded-but-off devices don't linger in the list. */
static void read_scan(void)
{
	char seen[BT_MAX * 2][20];
	int nseen = 0;
	FILE *sf = fopen(SEEN_F, "r");
	if (sf) {
		char line[20];
		while (nseen < (int)(sizeof seen / sizeof seen[0]) && fgets(line, sizeof line, sf)) {
			char *nl = strpbrk(line, "\r\n");
			if (nl) *nl = '\0';
			if (line[0]) snprintf(seen[nseen++], sizeof seen[0], "%s", line);
		}
		fclose(sf);
	}

	s_count = 0;
	FILE *f = fopen(SCAN_F, "r");
	if (!f) return;
	char line[256];
	while (s_count < BT_MAX && fgets(line, sizeof line, f)) {
		char mac[20], name[64];
		if (sscanf(line, "Device %19s %63[^\r\n]", mac, name) != 2) continue;
		if (name_is_addr(mac, name)) continue;
		int present = 0;
		for (int i = 0; i < nseen; i++)
			if (strcasecmp(seen[i], mac) == 0) { present = 1; break; }
		if (!present) continue;
		snprintf(s_list[s_count].mac, sizeof s_list[0].mac, "%s", mac);
		snprintf(s_list[s_count].name, sizeof s_list[0].name, "%s", name);
		s_count++;
	}
	fclose(f);
}

/* ---- flow ---- */

static void start_scan(void)
{
	s_count = s_cursor = 0;
	s_phase = PH_SCAN;
	s_phase_since = SDL_GetTicks();
	unlink(SEEN_F);
	unlink(SCAN_F);
	touch(PAUSE_F); /* hold off the reconnect loop while we drive D-Bus */
	/* Classic (BR/EDR) discovery -- an A2DP headset does not answer an LE scan.
	 * Capture the MACs seen this pass, then the name list. */
	child_spawn(
	    "{ echo 'menu scan'; echo 'transport bredr'; echo 'back'; echo 'scan on';"
	    " sleep 6; echo 'scan off'; echo 'quit'; } | bluetoothctl 2>&1"
	    " | grep -oiE '([0-9A-F]{2}:){5}[0-9A-F]{2}' | sort -u > " SEEN_F ";"
	    " bluetoothctl devices > " SCAN_F " 2>/dev/null");
}

static void start_pairing(const char *m)
{
	char cmd[640];
	s_phase = PH_PAIRING;
	s_phase_since = SDL_GetTicks();
	unlink(PAIR_F);
	touch(PAUSE_F);
	/* Kill any background music daemon before bonding: it caches bt_mac at
	 * startup and holds the switch/audio, so a stale one would keep routing to
	 * the old sink and race us on D-Bus. The next Muse launch respawns it fresh
	 * against the newly-bonded device. */
	system("killall -q muse 2>/dev/null");
	/* Full BR/EDR bond. Clear any stale keyless record first, rediscover on the
	 * classic transport, then pair+trust+connect. Judge success by the real
	 * Connected: yes state -- bluetoothctl connect returns Failed for a2dp even
	 * when the link comes up. */
	snprintf(cmd, sizeof cmd,
	    "{ echo 'power on'; echo 'agent NoInputNoOutput'; echo 'default-agent'; echo 'pairable on';"
	    " echo 'remove %s'; sleep 1;"
	    " echo 'menu scan'; echo 'transport bredr'; echo 'back'; echo 'scan on'; sleep 5; echo 'scan off';"
	    " echo 'pair %s'; sleep 8; echo 'trust %s'; echo 'connect %s'; sleep 5; echo 'quit'; }"
	    " | bluetoothctl >/dev/null 2>&1;"
	    " bluetoothctl info %s 2>/dev/null | grep -q 'Connected: yes'"
	    " && echo 0 > " PAIR_F " || echo 1 > " PAIR_F,
	    m, m, m, m, m);
	child_spawn(cmd);
}

static void dismiss(void)
{
	if (s_child > 0) { kill(s_child, SIGTERM); }
	unlink(PAUSE_F);
	s_dismissed = true;
	s_engaged = false;
	s_phase = PH_CONNECTING;
}

/* ---- public ---- */

void btpair_init(const char *cfg_path)
{
	snprintf(s_cfg, sizeof s_cfg, "%s", cfg_path);
	/* read the remembered sink once */
	FILE *f = fopen(cfg_path, "r");
	if (f) {
		char line[512];
		while (fgets(line, sizeof line, f)) {
			if (strncmp(line, "bt_mac=", 7) == 0) {
				char *v = line + 7, *nl = strpbrk(v, "\r\n");
				if (nl) *nl = '\0';
				snprintf(s_mac, sizeof s_mac, "%s", v);
			}
		}
		fclose(f);
	}
	s_f_title = TTF_OpenFont(P_FONT, 46);
	s_f_item = TTF_OpenFont(P_FONT, 40);
	s_f_hint = TTF_OpenFont(P_FONT, 30);
#ifdef __linux__
	s_sw_fd = open(SWITCH_DEV, O_RDONLY | O_NONBLOCK);
	if (s_sw_fd >= 0) s_want_bt = !sw_bit(s_sw_fd); /* switch up = BT */
#else
	s_want_bt = getenv("EROS_BT_SWITCH") != NULL; /* native dev: force on for testing */
#endif
}

void btpair_poll(void)
{
#ifdef __linux__
	if (s_sw_fd < 0) return;
	struct input_event ev;
	while (read(s_sw_fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
		if (ev.type == EV_SW && ev.code == SW_TABLET_MODE)
			s_want_bt = ev.value ? false : true; /* switch up = BT */
	}
#endif
}

void btpair_tick(void)
{
	Uint32 now = SDL_GetTicks();

	if (s_probe > 0) { int st; if (waitpid(s_probe, &st, WNOHANG) == s_probe) s_probe = -1; }

	if (!s_want_bt) {          /* switch not on BT: never show, reset */
		if (s_engaged) dismiss();
		s_engaged = false;
		s_dismissed = false;
		s_disc_since = 0;
		return;
	}
	if (bt_connected()) {      /* connected: no screen, clear dismissal */
		if (s_engaged && s_phase != PH_OK) { s_engaged = false; }
		s_dismissed = false;
		s_disc_since = 0;
		return;
	}
	/* switch on BT, not connected */
	if (s_disc_since == 0) s_disc_since = now;
	if (s_dismissed || s_engaged) return;
	if (now - s_disc_since < ENGAGE_DELAY) return;
	s_engaged = true;
	s_phase = PH_CONNECTING;
	s_phase_since = now;
}

bool btpair_active(void) { return s_engaged; }

const char *btpair_mac(void) { return s_mac; }

void btpair_update(in_state *in)
{
	Uint32 now = SDL_GetTicks();

	if (s_child > 0) {
		int st;
		if (waitpid(s_child, &st, WNOHANG) == s_child) {
			s_child = -1;
			if (s_phase == PH_SCAN) {
				read_scan();
				s_phase = PH_LIST;
				s_cursor = 0;
				s_phase_since = now;
				/* Stop holding off the reconnect loop while the list just sits
				 * waiting for a pick -- so if the sink reconnects on its own, the
				 * loop republishes the state and this screen auto-closes. */
				unlink(PAUSE_F);
			} else if (s_phase == PH_PAIRING) {
				int ok = 0;
				FILE *pf = fopen(PAIR_F, "r");
				if (pf) { int rc = 1; if (fscanf(pf, "%d", &rc) == 1) ok = (rc == 0); fclose(pf); }
				if (ok) {
					save_bt_mac(s_list[s_cursor].mac);
					write_state(1);   /* disengage immediately; loop keeps it fresh */
				}
				s_phase = ok ? PH_OK : PH_FAIL;
				s_phase_since = now;
				unlink(PAUSE_F);
			}
		}
	}

	/* While the screen is just waiting (no scan/pair child running), poll the link
	 * fast in the background so a reconnect closes the screen within a fraction of
	 * a second instead of on launch.sh's slower cadence. Read-only info -- can't
	 * collide with the loop's connect -- and only runs while this screen is up. */
	if (s_mac[0] && s_child < 0 && s_probe < 0 && now - s_probe_at > PROBE_MS &&
	    (s_phase == PH_CONNECTING || s_phase == PH_LIST || s_phase == PH_FAIL)) {
		char cmd[160];
		snprintf(cmd, sizeof cmd,
		    "bluetoothctl info %s 2>/dev/null | grep -q 'Connected: yes' && echo 1 > " STATE_F,
		    s_mac);
		s_probe_at = now;
		s_probe = fork();
		if (s_probe == 0) { execl("/bin/sh", "sh", "-c", cmd, (char *)NULL); _exit(127); }
	}

	switch (s_phase) {
	case PH_CONNECTING:
		if (in->pressed[IN_BACK]) { dismiss(); break; }
		if (in->pressed[IN_X]) { start_scan(); break; }
		if (now - s_phase_since > (s_mac[0] ? GRACE_MAC : GRACE_NODEV)) start_scan();
		break;
	case PH_SCAN:
		if (in->pressed[IN_BACK]) dismiss();
		break;
	case PH_LIST:
		if (s_count > 0) {
			if (in_repeat(in, IN_UP)) s_cursor = (s_cursor - 1 + s_count) % s_count;
			if (in_repeat(in, IN_DOWN)) s_cursor = (s_cursor + 1) % s_count;
			if (in->pressed[IN_ACCEPT]) start_pairing(s_list[s_cursor].mac);
		}
		if (in->pressed[IN_X]) start_scan();
		if (in->pressed[IN_BACK]) { unlink(PAUSE_F); dismiss(); }
		break;
	case PH_PAIRING:
		break; /* wait for the child */
	case PH_OK:
		if (now - s_phase_since > 1500 || in->pressed[IN_ACCEPT] || in->pressed[IN_BACK]) {
			s_engaged = false;
			s_dismissed = false;
		}
		break;
	case PH_FAIL:
		if (in->pressed[IN_ACCEPT]) start_scan();
		if (in->pressed[IN_BACK]) dismiss();
		break;
	}
}

/* ---- render ---- */

static void dtext(SDL_Renderer *r, TTF_Font *f, const char *s, int cx, int cy, SDL_Color col)
{
	if (!f || !s || !*s) return;
	SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s, col);
	if (!surf) return;
	SDL_Texture *t = SDL_CreateTextureFromSurface(r, surf);
	SDL_Rect dst = { cx - surf->w / 2, cy - surf->h / 2, surf->w, surf->h };
	if (t) { SDL_RenderCopy(r, t, NULL, &dst); SDL_DestroyTexture(t); }
	SDL_FreeSurface(surf);
}

void btpair_render(SDL_Renderer *r)
{
	SDL_Color white = { 235, 235, 240, 255 };
	SDL_Color dim = { 150, 150, 160, 255 };
	int cx = EROS_SCREEN_W / 2;

	SDL_SetRenderDrawColor(r, 10, 11, 16, 255);
	SDL_RenderClear(r);
	dtext(r, s_f_title, "Bluetooth", cx, 110, white);

	switch (s_phase) {
	case PH_CONNECTING:
		dtext(r, s_f_item, s_mac[0] ? "Connecting…" : "Bluetooth is on", cx, EROS_SCREEN_H / 2, dim);
		dtext(r, s_f_hint, "X: pair a device    B: cancel", cx, EROS_SCREEN_H - 90, dim);
		break;
	case PH_SCAN:
		dtext(r, s_f_item, "Searching for headphones…", cx, EROS_SCREEN_H / 2, dim);
		dtext(r, s_f_hint, "B: cancel", cx, EROS_SCREEN_H - 90, dim);
		break;
	case PH_LIST:
		if (s_count == 0) {
			dtext(r, s_f_item, "No headphones found", cx, EROS_SCREEN_H / 2 - 20, dim);
			dtext(r, s_f_hint, "Put them in pairing mode.", cx, EROS_SCREEN_H / 2 + 40, dim);
			dtext(r, s_f_hint, "X: rescan  B: cancel", cx, EROS_SCREEN_H - 90, dim);
		} else {
			int y = 250;
			for (int i = 0; i < s_count; i++) {
				dtext(r, s_f_item, s_list[i].name, cx, y, i == s_cursor ? white : dim);
				y += 58;
			}
			dtext(r, s_f_hint, "A: pair    X: rescan    B: cancel", cx, EROS_SCREEN_H - 90, dim);
		}
		break;
	case PH_PAIRING: {
		char msg[96];
		snprintf(msg, sizeof msg, "Pairing with %s…", s_list[s_cursor].name);
		dtext(r, s_f_item, msg, cx, EROS_SCREEN_H / 2, dim);
		break;
	}
	case PH_OK:
		dtext(r, s_f_item, "Connected", cx, EROS_SCREEN_H / 2, white);
		break;
	default: /* PH_FAIL */
		dtext(r, s_f_item, "Pairing failed", cx, EROS_SCREEN_H / 2 - 20, white);
		dtext(r, s_f_hint, "A: retry    B: cancel", cx, EROS_SCREEN_H / 2 + 40, dim);
		break;
	}

	plat_draw_osd(r);
	SDL_RenderPresent(r);
}
