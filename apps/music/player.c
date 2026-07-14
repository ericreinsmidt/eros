#include "player.h"
#include "audio.h"
#include "library.h"
#include "../../src/platform.h"   /* plat_settings_init / volume / brightness */

#include <SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>          /* EV_SW, SW_TABLET_MODE, EVIOCGSW, KEY_* */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CMD_FIFO   "/tmp/musicd.cmd"
#define STATUS_F   "/tmp/musicd.status"
#define PID_F      "/tmp/musicd.pid"

/* ---- daemon ---- */

static volatile sig_atomic_t d_quit;
static void d_on_term(int sig) { (void)sig; d_quit = 1; }

static mus_library d_lib;
static bool        d_have;
static int         d_track;
static int         d_active;
static char        d_dir[1024];
static int         d_mode;    /* 0 off (play through + stop), 1 repeat-all, 2 repeat-one, 3 shuffle */

static void bt_read_mac(void);   /* defined below; used by the btrefresh command */
static void bt_apply(void);

static void d_load(const char *dir)
{
	if (d_have && strcmp(d_dir, dir) == 0) return; /* same album already loaded */
	if (d_have) { library_free(&d_lib); d_have = false; }
	if (library_scan(dir, &d_lib)) {
		d_have = true;
		snprintf(d_dir, sizeof d_dir, "%s", dir);
	}
}

/* advance to the next/prev playable track in the loaded album */
static void d_step(int delta)
{
	if (!d_have) return;
	int n = d_lib.albums[0].track_count;
	int dir = delta < 0 ? -1 : 1;
	for (int i = 1; i <= n; i++) {
		int t = ((d_track + dir * i) % n + n) % n;
		if (audio_play(d_lib.albums[0].tracks[t].path)) { d_track = t; return; }
	}
}

/* play track `t`, falling through to the next playable one if it won't open */
static void d_play_index(int t)
{
	if (!d_have) return;
	int n = d_lib.albums[0].track_count;
	for (int i = 0; i < n; i++) {
		int k = ((t + i) % n + n) % n;
		if (audio_play(d_lib.albums[0].tracks[k].path)) { d_track = k; return; }
	}
}

/* a random track index other than the current one (for shuffle) */
static int d_rand_other(void)
{
	int n = d_have ? d_lib.albums[0].track_count : 0;
	if (n <= 1) return 0;
	int r = rand() % n;
	if (r == d_track) r = (r + 1) % n;
	return r;
}

/* what to play when the current track ends, per the repeat/shuffle mode */
static void d_auto_advance(void)
{
	int n = d_have ? d_lib.albums[0].track_count : 0;
	switch (d_mode) {
	case 1: d_step(+1); break;                       /* repeat all: loop the album */
	case 2: d_play_index(d_track); break;            /* repeat one */
	case 3: d_play_index(d_rand_other()); break;     /* shuffle */
	default:                                          /* 0 = off: play through, then stop */
		if (d_track + 1 < n) d_play_index(d_track + 1);
		else { audio_stop(); d_active = 0; }         /* end of album -> stop */
		break;
	}
}

static void d_handle(char *line)
{
	char *cmd = line;
	char *arg = strchr(line, '\t');
	if (arg) *arg++ = '\0';

	if (!strcmp(cmd, "play")) {
		/* arg = "<dir>\t<track>\t<pos>" */
		char *p2 = arg ? strchr(arg, '\t') : NULL;
		if (!p2) return;
		*p2++ = '\0';
		char *p3 = strchr(p2, '\t');
		double pos = 0;
		if (p3) { *p3++ = '\0'; pos = atof(p3); }
		int track = atoi(p2);
		d_load(arg);
		if (d_have) {
			int n = d_lib.albums[0].track_count;
			if (track < 0 || track >= n) track = 0;
			if (audio_play(d_lib.albums[0].tracks[track].path)) {
				d_track = track;
				d_active = 1;
				if (pos > 0.5) audio_seek(pos);
			}
		}
	} else if (!strcmp(cmd, "pause")) {
		audio_pause();
	} else if (!strcmp(cmd, "resume")) {
		audio_resume();
	} else if (!strcmp(cmd, "next")) {
		if (d_mode == 3) d_play_index(d_rand_other());   /* shuffle: manual skip is random too */
		else d_step(+1);
	} else if (!strcmp(cmd, "prev")) {
		d_step(-1);                                       /* prev stays sequential in every mode */
	} else if (!strcmp(cmd, "mode")) {
		if (arg) { int m = atoi(arg); if (m >= 0 && m <= 3) d_mode = m; }
	} else if (!strcmp(cmd, "seek")) {
		if (arg) audio_seek(atof(arg));
	} else if (!strcmp(cmd, "output")) {
		if (arg) audio_set_device(arg);   /* "default" or "bluealsa:DEV=..,PROFILE=a2dp" */
	} else if (!strcmp(cmd, "btrefresh")) {
		bt_read_mac(); bt_apply();        /* UI paired a new sink -> re-read + route */
	} else if (!strcmp(cmd, "vol")) {
		if (arg) plat_volume_nudge(atoi(arg));
	} else if (!strcmp(cmd, "bright")) {
		if (arg) plat_brightness_nudge(atoi(arg));
	} else if (!strcmp(cmd, "stop")) {
		audio_stop();
		d_active = 0;
	} else if (!strcmp(cmd, "quit")) {
		d_quit = 1;
	}
}

/* ---- Bluetooth output, driven by the physical SW_TABLET_MODE switch ----
 * The switch on event3 selects the audio output: one position = speaker, the
 * other = the remembered BT sink (`bt_mac` in eros.cfg). On flip to BT we make
 * sure the sink is connected and point the audio backend at its bluealsa PCM; if
 * it can't be reached we fall back to the speaker (d_bt_note tells the UI). */
#define SWITCH_DEV "/dev/input/event3"
#define EROS_CFG   "/mnt/SDCARD/eros/eros.cfg"
#define BT_CONN_F  "/tmp/eros_bt_conn"   /* background reconnect writes "1" here */

static int  d_sw_fd = -1;
static int  d_want_bt = 0;         /* switch position: 1 = BT, 0 = speaker */
static char d_bt_mac[32] = "";
static int  d_bt_note = 0;         /* 1 = asked for BT but it's unavailable */
static int  bt_tick_ctr = 0;       /* throttles the background-reconnect re-check */
static int  d_avrcp_fd = -1;       /* headset AVRCP media-button input device */

static void bt_read_mac(void)
{
	FILE *f = fopen(EROS_CFG, "r");
	if (!f) return;
	char line[128];
	while (fgets(line, sizeof line, f)) {
		if (strncmp(line, "bt_mac=", 7) == 0) {
			char *v = line + 7, *nl = strpbrk(v, "\r\n");
			if (nl) *nl = '\0';
			snprintf(d_bt_mac, sizeof d_bt_mac, "%s", v);
			break;
		}
	}
	fclose(f);
}

/* current position of SW_TABLET_MODE (evdev doesn't send it on open) */
static int sw_state(int fd)
{
	unsigned long bits[(SW_MAX + 1 + 8 * sizeof(long) - 1) / (8 * sizeof(long))];
	memset(bits, 0, sizeof bits);
	if (ioctl(fd, EVIOCGSW(sizeof bits), bits) < 0) return 0;
	return (bits[SW_TABLET_MODE / (8 * sizeof(long))] >> (SW_TABLET_MODE % (8 * sizeof(long)))) & 1;
}

static int bt_connected(void)
{
	if (!d_bt_mac[0]) return 0;
	char cmd[96];
	snprintf(cmd, sizeof cmd, "bluetoothctl info %s 2>/dev/null | grep -q 'Connected: yes'", d_bt_mac);
	return system(cmd) == 0;
}

/* Point the audio backend at the speaker or the BT sink per the switch. */
static void bt_route_to_sink(void)
{
	char dev[64];
	snprintf(dev, sizeof dev, "bluealsa:DEV=%s,PROFILE=a2dp", d_bt_mac);
	audio_set_device(dev);
	d_bt_note = 0;
}

static void bt_apply(void)
{
	if (!d_want_bt) { audio_set_device("default"); d_bt_note = 0; return; }

	/* Already connected (the common case -- the sink stays connected across
	 * speaker<->BT flips) -> route instantly, no pairing screen. */
	int conn = d_bt_mac[0] ? bt_connected() : 0;
	if (conn) { bt_route_to_sink(); return; }

	/* Not connected: flag the BT/pairing screen NOW (so it appears the instant
	 * the switch flips) and, if we have a remembered sink, try to reconnect it
	 * in the BACKGROUND. The child writes a result file on success; bt_tick()
	 * (a cheap file check) routes + dismisses the screen when it lands.
	 * Do NOT poll bluetoothctl on a timer here -- forking a D-Bus process every
	 * couple seconds starved the ALSA thread and caused audio dropouts. */
	audio_set_device("default");
	d_bt_note = 1;
	if (d_bt_mac[0]) {
		/* (Re)assert trust so BlueZ auto-reconnects the sink on its own, then retry
		 * connect a few times (the headset needs a moment to become connectable
		 * after a power-cycle). CRUCIAL: judge success by the device's actual
		 * `Connected: yes` state, NOT by `bluetoothctl connect`'s exit code --
		 * BlueZ often returns org.bluez.Error.Failed for the a2dp profile even
		 * though the link came up, and keying on the exit code made a genuinely
		 * reconnected headset look like a failure (-> stuck on the pairing screen).
		 * All in a detached subshell so the ALSA thread never stalls on D-Bus. */
		char cmd[384];
		snprintf(cmd, sizeof cmd,
		         "rm -f %s; (bluetoothctl trust %s >/dev/null 2>&1; "
		         "for i in 1 2 3 4 5; do bluetoothctl connect %s >/dev/null 2>&1; "
		         "bluetoothctl info %s 2>/dev/null | grep -q 'Connected: yes' && { echo 1 > %s; break; }; "
		         "sleep 2; done) &",
		         BT_CONN_F, d_bt_mac, d_bt_mac, d_bt_mac, BT_CONN_F);
		system(cmd);   /* '&' -> returns immediately */
	}
}

/* Cheap periodic check: if the background reconnect wrote its success file, route
 * to the sink and dismiss the pairing screen. No forking here. */
static void bt_tick(void)
{
	if (!(d_want_bt && d_bt_mac[0] && d_bt_note)) return;
	FILE *f = fopen(BT_CONN_F, "r");
	if (!f) return;
	int ok = 0;
	if (fscanf(f, "%d", &ok) != 1) ok = 0;
	fclose(f);
	if (ok) { unlink(BT_CONN_F); bt_route_to_sink(); }
}

/* Find and open the headset's AVRCP passthrough input device. BlueZ creates one
 * named "<device> (AVRCP)" on connect; its event number varies and it appears/
 * disappears with the connection, so we locate it by name rather than hard-code
 * an event node. */
static int avrcp_open(void)
{
	for (int i = 0; i < 32; i++) {
		char path[32];
		snprintf(path, sizeof path, "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0) continue;
		char name[128] = "";
		if (ioctl(fd, EVIOCGNAME(sizeof name), name) >= 0 && strstr(name, "(AVRCP)"))
			return fd;
		close(fd);
	}
	return -1;
}

/* React to the headset's media button. The headset does the press-counting and
 * sends distinct AVRCP keys: single = play/pause, double = next, triple = prev.
 * Act only on key-down (value 1). */
static void avrcp_poll(void)
{
	if (d_avrcp_fd < 0) return;
	struct input_event ev;
	ssize_t r;
	while ((r = read(d_avrcp_fd, &ev, sizeof ev)) == (ssize_t)sizeof ev) {
		if (ev.type != EV_KEY || ev.value != 1) continue;
		switch (ev.code) {
		case KEY_PLAYPAUSE: case KEY_PLAYCD: case KEY_PAUSECD: case KEY_PLAY:
			audio_toggle_pause();
			break;
		case KEY_NEXTSONG:
			if (d_mode == 3) d_play_index(d_rand_other()); else d_step(+1);
			break;
		case KEY_PREVIOUSSONG:
			d_step(-1);
			break;
		}
	}
	if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		close(d_avrcp_fd);   /* headset disconnected -> device vanished; bt_tick re-opens */
		d_avrcp_fd = -1;
	}
}

static void bt_init(void)
{
	bt_read_mac();
	d_sw_fd = open(SWITCH_DEV, O_RDONLY | O_NONBLOCK);
	if (d_sw_fd >= 0) d_want_bt = !sw_state(d_sw_fd);   /* switch up = BT */
	d_avrcp_fd = avrcp_open();
	bt_apply();
}

static void bt_poll(void)
{
	if (d_sw_fd < 0) return;
	struct input_event ev;
	int changed = 0;
	while (read(d_sw_fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
		if (ev.type == EV_SW && ev.code == SW_TABLET_MODE) {
			d_want_bt = ev.value ? 0 : 1;   /* switch up = BT */
			changed = 1;
		}
	}
	if (changed) bt_apply();
}

static bool daemon_alive(void)
{
	FILE *f = fopen(PID_F, "r");
	if (!f) return false;
	int p = 0;
	int ok = (fscanf(f, "%d", &p) == 1);
	fclose(f);
	return ok && p > 0 && kill((pid_t)p, 0) == 0;
}

int player_daemon_main(void)
{
	if (daemon_alive()) return 0; /* another daemon already owns audio */
	FILE *pf = fopen(PID_F, "w");
	if (pf) { fprintf(pf, "%d", (int)getpid()); fclose(pf); }

	signal(SIGTERM, d_on_term);
	signal(SIGINT, d_on_term);
	srand((unsigned)(time(NULL) ^ getpid()));   /* shuffle randomness */
	if (!audio_init()) return 1;
	/* Own volume/brightness here, in the persistent daemon. InitSettings runs
	 * once, at daemon birth, BEFORE any audio plays -- so its codec re-init is
	 * inaudible. (The UI client used to call it on every launch, which popped the
	 * volume mid-playback.) */
	plat_settings_init();
	bt_init();   /* read the BT switch position + route accordingly */

	mkfifo(CMD_FIFO, 0666);
	int fd = open(CMD_FIFO, O_RDWR | O_NONBLOCK); /* O_RDWR: stays readable across writers */

	char line[1024];
	size_t ll = 0;
	while (!d_quit) {
		char rd[512];
		ssize_t n;
		while (fd >= 0 && (n = read(fd, rd, sizeof rd)) > 0) {
			for (ssize_t i = 0; i < n; i++) {
				if (rd[i] == '\n' || ll >= sizeof line - 1) {
					line[ll] = '\0';
					if (ll) d_handle(line);
					ll = 0;
				} else {
					line[ll++] = rd[i];
				}
			}
		}

		bt_poll();     /* react to the physical BT output switch */
		avrcp_poll();  /* react to the headset's media buttons */
		if (++bt_tick_ctr >= 10) {   /* ~0.5s */
			bt_tick_ctr = 0;
			bt_tick();
			if (d_avrcp_fd < 0) d_avrcp_fd = avrcp_open();   /* (re)attach after a reconnect */
		}

		if (d_active && audio_finished()) d_auto_advance(); /* repeat/shuffle-aware */

		/* Write atomically (temp + rename) so the client -- which polls this many
		 * times per daemon write -- never catches a half-written file. A partial
		 * read used to flicker bt_note and thrash the pairing screen. */
		FILE *s = fopen(STATUS_F ".tmp", "w");
		if (s) {
			fprintf(s, "%d %.2f %.2f %d %d %d %d %d %d\n%s\n",
			        d_track, audio_position(), audio_duration(),
			        audio_is_paused() ? 1 : 0, d_active,
			        plat_volume_get(), plat_brightness_get(), d_bt_note, d_mode,
			        d_have ? d_dir : "");
			fclose(s);
			rename(STATUS_F ".tmp", STATUS_F);
		}
		SDL_Delay(50);
	}

	audio_stop();
	if (d_have) library_free(&d_lib);
	if (fd >= 0) close(fd);
	unlink(CMD_FIFO);
	unlink(STATUS_F);
	unlink(PID_F);
	return 0;
}

/* ---- client ---- */

static char self_path[1024];
void player_set_self(const char *argv0)
{
	if (argv0 && argv0[0]) snprintf(self_path, sizeof self_path, "%s", argv0);
}

void player_ensure_running(void)
{
	if (daemon_alive()) return;
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		/* Linux: /proc/self/exe is exact; elsewhere (native dev) fall back to argv0 */
		const char *exe = (access("/proc/self/exe", X_OK) == 0) ? "/proc/self/exe"
		                  : (self_path[0] ? self_path : "muse");
		execl(exe, "muse", "--daemon", (char *)NULL);
		_exit(127);
	}
	for (int i = 0; i < 60 && !daemon_alive(); i++) usleep(25000); /* wait up to ~1.5s */
}

static void player_send(const char *line)
{
	int fd = open(CMD_FIFO, O_WRONLY | O_NONBLOCK);
	if (fd < 0) return;
	char buf[1100];
	int len = snprintf(buf, sizeof buf, "%s\n", line);
	if (write(fd, buf, (size_t)len) < 0) { /* ignore: daemon buffers */ }
	close(fd);
}

void player_play(const char *dir, int track, double pos)
{
	char buf[1100];
	snprintf(buf, sizeof buf, "play\t%s\t%d\t%.2f", dir, track, pos);
	player_send(buf);
}
void player_pause(void)  { player_send("pause"); }
void player_resume(void) { player_send("resume"); }
void player_next(void)   { player_send("next"); }
void player_prev(void)   { player_send("prev"); }
void player_stop(void)   { player_send("stop"); }
void player_quit(void)   { player_send("quit"); }
void player_seek(double delta)
{
	char buf[64];
	snprintf(buf, sizeof buf, "seek\t%.2f", delta);
	player_send(buf);
}
void player_volume(int delta)
{
	char buf[32];
	snprintf(buf, sizeof buf, "vol\t%d", delta);
	player_send(buf);
}
void player_brightness(int delta)
{
	char buf[32];
	snprintf(buf, sizeof buf, "bright\t%d", delta);
	player_send(buf);
}
void player_bt_refresh(void) { player_send("btrefresh"); }
void player_mode(int mode)
{
	char buf[32];
	snprintf(buf, sizeof buf, "mode\t%d", mode);
	player_send(buf);
}

bool player_get(player_state *st)
{
	FILE *f = fopen(STATUS_F, "r");
	if (!f) return false;
	memset(st, 0, sizeof *st);
	st->vol = st->bright = -1;
	int paused = 0, active = 0;
	int got = fscanf(f, "%d %lf %lf %d %d %d %d %d %d\n",
	                 &st->track, &st->pos, &st->dur, &paused, &active,
	                 &st->vol, &st->bright, &st->bt_note, &st->mode);
	if (got >= 5) {
		if (!fgets(st->dir, sizeof st->dir, f)) st->dir[0] = '\0';
		char *nl = strchr(st->dir, '\n');
		if (nl) *nl = '\0';
	}
	fclose(f);
	st->paused = paused != 0;
	st->active = active != 0;
	return got >= 5;
}
