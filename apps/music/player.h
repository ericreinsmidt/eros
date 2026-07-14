#ifndef EROS_MUSE_PLAYER_H
#define EROS_MUSE_PLAYER_H

#include <stdbool.h>

/* Background music daemon (musicd) + its client API. Audio lives entirely in the
 * daemon process, which keeps the ALSA/SDL device open across tracks -- so the
 * Muse UI can come and go without ever interrupting playback (gapless
 * backgrounding: closing the UI simply leaves the daemon streaming). The UI
 * drives the daemon through a command FIFO and reads a small status file. */

int  player_daemon_main(void);       /* run the daemon (muse --daemon) */
void player_set_self(const char *argv0); /* how to re-exec this binary (native fallback) */
void player_ensure_running(void);    /* spawn the daemon if it isn't already up */

void player_play(const char *dir, int track, double pos); /* play album dir from track@pos */
void player_pause(void);
void player_resume(void);
void player_next(void);
void player_prev(void);
void player_seek(double delta_seconds);
void player_stop(void);               /* stop playback; daemon stays alive */
void player_quit(void);               /* ask the daemon to exit */

/* Volume/brightness are owned by the daemon (it calls InitSettings once, before
 * any audio, so re-opening the UI never re-inits the codec and pops). The client
 * nudges them through the daemon and reads the current levels back from status. */
void player_volume(int delta);        /* -N..+N volume steps (0..20 scale) */
void player_brightness(int delta);    /* -N..+N brightness steps (0..10 scale) */
void player_bt_refresh(void);         /* re-read bt_mac + re-route (after pairing) */
void player_mode(int mode);           /* 0 off (album ends), 1 repeat-all, 2 repeat-one, 3 shuffle */

typedef struct {
	int    track;   /* current track index in the album */
	double pos;     /* seconds into the track */
	double dur;     /* track length, seconds (0 = unknown) */
	bool   paused;
	bool   active;  /* something is loaded/playing */
	int    vol;     /* current volume 0..20 (-1 unknown) */
	int    bright;  /* current brightness 0..10 (-1 unknown) */
	int    bt_note; /* 1 = BT requested (switch up) but no sink reachable */
	int    mode;    /* 0 off (album ends), 1 repeat-all, 2 repeat-one, 3 shuffle */
	char   dir[1024]; /* album folder currently loaded */
} player_state;

/* Fill *st from the daemon's status file. Returns false if no daemon/status. */
bool player_get(player_state *st);

#endif /* EROS_MUSE_PLAYER_H */
