#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <dirent.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "btpair.h"
#include "config.h"
#include "coverflow.h"
#include "platform.h"

#define GAME_TEX_KEEP 8 /* evict box-art textures farther than this */
#define BATT_LOW_PCT 10 /* show the low-battery dot at or below this */
#define BT_FIFO "/tmp/eros_af" /* game audio -> aplay forwarder -> BT sink */

typedef struct {
	char name[CFG_STR];     /* display name: filename/folder without extension */
	char file[CFG_STR * 2]; /* launch path relative to Roms/<system>/: a filename,
	                         * or "<folder>/<disc>" for a disc-folder game */
} game_entry;

typedef enum { SCREEN_SYSTEMS, SCREEN_GAMES } screen_id;

typedef struct {
	systems_cfg sys;
	eros_cfg cfg;
	TTF_Font *font_big;

	SDL_Texture *sys_tex[CFG_MAX_CARDS];
	int sys_w[CFG_MAX_CARDS], sys_h[CFG_MAX_CARDS];

	game_entry *games;
	SDL_Texture **game_tex;
	int *game_w, *game_h;
	int game_count;

	screen_id screen;
	int sys_cursor, game_cursor;
	coverflow cf_sys, cf_games;
	in_state in;
	bool running;
} app;

static volatile sig_atomic_t want_shot;
static volatile sig_atomic_t want_quit;
static void on_sigusr1(int sig) { (void)sig; want_shot = 1; }
static void on_sigterm(int sig) { (void)sig; want_quit = 1; }

/* EROS_SCRIPT="right,right,a,shot,b,quit" — one action every few frames,
 * for driving the UI without hands (dev/testing only). */
static char script_buf[1024];
static char *script_next;

static void script_init(void)
{
	const char *s = getenv("EROS_SCRIPT");
	if (!s) return;
	snprintf(script_buf, sizeof script_buf, "%s", s);
	script_next = script_buf;
}

static void script_step(in_state *in)
{
	static int cooldown;
	if (!script_next || !*script_next) return;
	if (++cooldown < 20) return;
	cooldown = 0;
	char *tok = script_next;
	char *comma = strchr(tok, ',');
	if (comma) { *comma = '\0'; script_next = comma + 1; }
	else script_next = tok + strlen(tok);

	if (strcmp(tok, "left") == 0) in->pressed[IN_LEFT] = true;
	else if (strcmp(tok, "right") == 0) in->pressed[IN_RIGHT] = true;
	else if (strcmp(tok, "up") == 0) in->pressed[IN_UP] = true;
	else if (strcmp(tok, "down") == 0) in->pressed[IN_DOWN] = true;
	else if (strcmp(tok, "a") == 0) in->pressed[IN_ACCEPT] = true;
	else if (strcmp(tok, "b") == 0) in->pressed[IN_BACK] = true;
	else if (strcmp(tok, "shot") == 0) want_shot = 1;
	else if (strcmp(tok, "quit") == 0) in->quit_requested = true;
	/* "wait" (or anything else) just burns a step */
}

static void save_shot(SDL_Renderer *r)
{
	static int shot_no;
	/* GLES can only read back RGBA; ask for what the renderer prefers */
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, EROS_SCREEN_W, EROS_SCREEN_H,
	                                                32, SDL_PIXELFORMAT_RGBA32);
	if (!s) return;
	if (SDL_RenderReadPixels(r, NULL, s->format->format, s->pixels, s->pitch) == 0) {
		char path[64];
		snprintf(path, sizeof path, "/tmp/eros_shot_%d.bmp", shot_no++);
		SDL_SaveBMP(s, path);
	} else {
		fprintf(stderr, "shot: %s\n", SDL_GetError());
	}
	SDL_FreeSurface(s);
}

/* A stand-in card texture: dark slab with the name on it. */
static SDL_Texture *make_fallback_card(SDL_Renderer *r, TTF_Font *font,
                                       const char *name, int *w, int *h)
{
	int cw = 420, ch = 560;
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, cw, ch, 32,
	                                                SDL_PIXELFORMAT_ARGB8888);
	if (!s) return NULL;
	SDL_FillRect(s, NULL, SDL_MapRGBA(s->format, 38, 40, 48, 255));
	SDL_Rect edge = { 0, 0, cw, 6 };
	Uint32 accent = SDL_MapRGBA(s->format, 96, 100, 116, 255);
	SDL_FillRect(s, &edge, accent);
	edge.y = ch - 6;
	SDL_FillRect(s, &edge, accent);
	if (font) {
		SDL_Color col = { 225, 225, 230, 255 };
		SDL_Surface *txt = TTF_RenderUTF8_Blended_Wrapped(font, name, col, cw - 60);
		if (txt) {
			SDL_Rect dst = { (cw - txt->w) / 2, (ch - txt->h) / 2, txt->w, txt->h };
			SDL_BlitSurface(txt, NULL, s, &dst);
			SDL_FreeSurface(txt);
		}
	}
	SDL_Texture *t = SDL_CreateTextureFromSurface(r, s);
	if (t) { *w = cw; *h = ch; }
	SDL_FreeSurface(s);
	return t;
}

static SDL_Texture *load_image(SDL_Renderer *r, const char *path, int *w, int *h)
{
	SDL_Texture *t = IMG_LoadTexture(r, path);
	if (t) SDL_QueryTexture(t, NULL, NULL, w, h);
	return t;
}

/* ---- texture caches ---- */

static SDL_Texture *sys_get_tex(void *ctx, int i, int *w, int *h)
{
	app *a = ctx;
	if (!a->sys_tex[i]) {
		char path[CFG_STR * 2];
		snprintf(path, sizeof path, "%s/icons/%s", P_ROOT, a->sys.cards[i].icon);
		a->sys_tex[i] = load_image(plat_renderer(), path, &a->sys_w[i], &a->sys_h[i]);
		if (!a->sys_tex[i])
			a->sys_tex[i] = make_fallback_card(plat_renderer(), a->font_big,
			                                   a->sys.cards[i].name,
			                                   &a->sys_w[i], &a->sys_h[i]);
	}
	*w = a->sys_w[i];
	*h = a->sys_h[i];
	return a->sys_tex[i];
}

static const char *cur_folder(app *a)
{
	return a->sys.cards[a->sys_cursor].target;
}

static SDL_Texture *game_get_tex(void *ctx, int i, int *w, int *h)
{
	app *a = ctx;
	if (!a->game_tex[i]) {
		char base[CFG_STR];
		snprintf(base, sizeof base, "%s", a->games[i].name);
		char path[CFG_STR * 3];
		snprintf(path, sizeof path, "%s/%s/.media/%s.png", P_ROMS, cur_folder(a), base);
		a->game_tex[i] = load_image(plat_renderer(), path, &a->game_w[i], &a->game_h[i]);
		if (!a->game_tex[i])
			a->game_tex[i] = make_fallback_card(plat_renderer(), a->font_big,
			                                    a->games[i].name,
			                                    &a->game_w[i], &a->game_h[i]);
	}
	*w = a->game_w[i];
	*h = a->game_h[i];
	return a->game_tex[i];
}

static void evict_far_game_tex(app *a)
{
	for (int i = 0; i < a->game_count; i++) {
		int d = abs(i - a->game_cursor);
		if (a->game_count >= CF_WINDOW)
			d = d > a->game_count / 2 ? a->game_count - d : d;
		if (a->game_tex[i] && d > GAME_TEX_KEEP) {
			SDL_DestroyTexture(a->game_tex[i]);
			a->game_tex[i] = NULL;
		}
	}
}

/* Decode the box art across the initial visible window (plus a small margin)
 * up front, so the first scroll moves don't hitch while a PNG decodes in the
 * middle of the slide animation. Cheap once warm: GAME_TEX_KEEP holds them. */
static void prime_game_window(app *a)
{
	if (a->game_count <= 0) return;
	int span = CF_HALF_WINDOW + 2;
	for (int k = -span; k <= span; k++) {
		int i = a->game_cursor + k;
		if (a->game_count >= 2) {
			i %= a->game_count;
			if (i < 0) i += a->game_count;
		} else if (i < 0 || i >= a->game_count) {
			continue;
		}
		int w, h;
		game_get_tex(a, i, &w, &h);
	}
}

/* Same idea for the system-icon row: launching a game or app tears every texture
 * down for the video handoff, so warm the visible span back up before the first
 * scroll (this is the hitch you feel on returning from Muse). Idempotent --
 * already-loaded icons are skipped, so it's cheap to call on every transition. */
static void prime_sys_window(app *a)
{
	if (a->sys.count <= 0) return;
	int span = CF_HALF_WINDOW + 2;
	for (int k = -span; k <= span; k++) {
		int i = a->sys_cursor + k;
		if (a->sys.count >= 2) {
			i %= a->sys.count;
			if (i < 0) i += a->sys.count;
		} else if (i < 0 || i >= a->sys.count) {
			continue;
		}
		int w, h;
		sys_get_tex(a, i, &w, &h);
	}
}

static void free_all_textures(app *a)
{
	for (int i = 0; i < a->sys.count; i++) {
		if (a->sys_tex[i]) { SDL_DestroyTexture(a->sys_tex[i]); a->sys_tex[i] = NULL; }
	}
	for (int i = 0; i < a->game_count; i++) {
		if (a->game_tex[i]) { SDL_DestroyTexture(a->game_tex[i]); a->game_tex[i] = NULL; }
	}
}

/* ---- game list ---- */

static int game_cmp(const void *pa, const void *pb)
{
	return strcasecmp(((const game_entry *)pa)->name, ((const game_entry *)pb)->name);
}

static void free_games(app *a)
{
	for (int i = 0; i < a->game_count; i++) {
		if (a->game_tex[i]) SDL_DestroyTexture(a->game_tex[i]);
	}
	free(a->games); a->games = NULL;
	free(a->game_tex); a->game_tex = NULL;
	free(a->game_w); a->game_w = NULL;
	free(a->game_h); a->game_h = NULL;
	a->game_count = 0;
}

/* Disc-image extensions, in launch preference. A folder holding one of these is
 * a disc-based game (PlayStation, etc.); .m3u wins so a multi-disc set launches
 * as one entry with disc-switching. */
static const char *DISC_EXTS[] = { "m3u", "chd", "cue", "pbp", "iso", "img", "toc", "mdf", "cbn", "bin", NULL };

static int disc_rank(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot) return -1;
	for (int i = 0; DISC_EXTS[i]; i++)
		if (strcasecmp(dot + 1, DISC_EXTS[i]) == 0) return i;
	return -1;
}

/* Choose the file to launch inside a game folder: the best-ranked disc image
 * (m3u first, then chd/cue/...), ties broken alphabetically so a discs-only
 * folder lands on Disc 1. Writes the bare filename to out; false if none. */
static bool folder_launch_file(const char *dirpath, char *out, size_t outsz)
{
	DIR *d = opendir(dirpath);
	if (!d) return false;
	int best = 9999;
	char pick[CFG_STR] = "";
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		int r = disc_rank(e->d_name);
		if (r < 0) continue;
		if (r < best || (r == best && strcasecmp(e->d_name, pick) < 0)) {
			best = r;
			snprintf(pick, sizeof pick, "%s", e->d_name);
		}
	}
	closedir(d);
	if (!pick[0]) return false;
	snprintf(out, outsz, "%s", pick);
	return true;
}

static bool load_games(app *a, const char *folder)
{
	free_games(a);
	char dirpath[CFG_STR * 2];
	snprintf(dirpath, sizeof dirpath, "%s/%s", P_ROMS, folder);
	DIR *d = opendir(dirpath);
	if (!d) return false;
	int cap = 64, n = 0;
	game_entry *list = malloc(cap * sizeof *list);
	struct dirent *e;

	/* Pass 1: top-level files are games, launched directly -- a NES .zip, a bare
	 * single-disc .chd, a top-level .m3u, etc. `file` is the filename; `name` is
	 * it without the extension. */
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char full[CFG_STR * 3];
		snprintf(full, sizeof full, "%s/%s", dirpath, e->d_name);
		struct stat st;
		if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
		if (n == cap) { cap *= 2; list = realloc(list, cap * sizeof *list); }
		snprintf(list[n].file, sizeof list[n].file,"%s", e->d_name);
		snprintf(list[n].name, CFG_STR, "%s", e->d_name);
		char *dot = strrchr(list[n].name, '.');
		if (dot && dot != list[n].name) *dot = '\0';
		n++;
	}

	/* Pass 2: a subfolder holding a disc image is a game too -- one entry named
	 * after the folder, launching the disc inside (the .m3u for multi-disc). Skip
	 * a folder shadowed by a same-named top-level file so the two layouts can't
	 * double up. `file` becomes the folder-relative path "<folder>/<disc>". */
	int files_n = n;
	rewinddir(d);
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char full[CFG_STR * 3];
		snprintf(full, sizeof full, "%s/%s", dirpath, e->d_name);
		struct stat st;
		if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
		bool shadowed = false;
		for (int k = 0; k < files_n; k++)
			if (strcasecmp(list[k].name, e->d_name) == 0) { shadowed = true; break; }
		if (shadowed) continue;
		char inside[CFG_STR];
		if (!folder_launch_file(full, inside, sizeof inside)) continue;
		if (n == cap) { cap *= 2; list = realloc(list, cap * sizeof *list); }
		snprintf(list[n].file, sizeof list[n].file,"%s/%s", e->d_name, inside);
		snprintf(list[n].name, CFG_STR, "%s", e->d_name);
		n++;
	}
	closedir(d);
	if (n == 0) { free(list); return false; }
	qsort(list, n, sizeof *list, game_cmp);
	a->games = list;
	a->game_count = n;
	a->game_tex = calloc(n, sizeof *a->game_tex);
	a->game_w = calloc(n, sizeof *a->game_w);
	a->game_h = calloc(n, sizeof *a->game_h);
	return true;
}

/* ---- launching ---- */

/* The power-off send-off, shown briefly before the launcher exits (launch.sh then
 * runs poweroff). Matches minarch's in-game "We'll always have Paris." so every
 * power-off is consistent, in a game or not. Assumes video is up. */
static void show_poweroff_message(app *a)
{
	SDL_Renderer *r = plat_renderer();
	if (!r) return;
	SDL_SetRenderDrawColor(r, 10, 11, 16, 255);
	SDL_RenderClear(r);
	/* A dedicated large font for the send-off -- font_big (34pt) is the card font
	 * and reads too small full-screen. Fall back to it if the open fails. */
	TTF_Font *f = TTF_OpenFont(P_FONT, 56);
	if (!f) f = a->font_big;
	if (f) {
		SDL_Color col = { 235, 235, 240, 255 };
		SDL_Surface *s = TTF_RenderUTF8_Blended(f, "We'll always have Paris.", col);
		if (s) {
			SDL_Texture *t = SDL_CreateTextureFromSurface(r, s);
			if (t) {
				SDL_Rect dst = { (EROS_SCREEN_W - s->w) / 2, (EROS_SCREEN_H - s->h) / 2, s->w, s->h };
				SDL_RenderCopy(r, t, NULL, &dst);
				SDL_DestroyTexture(t);
			}
			SDL_FreeSurface(s);
		}
		if (f != a->font_big) TTF_CloseFont(f);
	}
	SDL_RenderPresent(r);
	SDL_Delay(1500);
}

static void run_child_with_video_handoff(app *a, char *const argv[],
                                         const char *const envkv[],
                                         const char *workdir)
{
	free_all_textures(a);
	plat_input_quit();
	plat_video_quit();
	int rc = plat_run(argv, envkv, workdir);
	fprintf(stderr, "child %s exited %d\n", argv[0], rc);
	/* A child app (Muse, a game) can request power-off by writing the flag and
	 * exiting; honor it on return instead of falling back to Cover Flow. Exiting
	 * the launcher lets launch.sh see the flag and run poweroff. Re-init video just
	 * long enough to show the send-off (it was torn down for the child handoff). */
	if (access(EROS_POWEROFF_FLAG, F_OK) == 0) {
		if (plat_video_init()) show_poweroff_message(a);
		a->running = false;
		return;
	}
	if (!plat_video_init() || !plat_input_init()) {
		/* display didn't come back; let the launch loop restart us */
		a->running = false;
		return;
	}
	plat_leds_off(); /* a core may have lit LEDs; kill them again */
	memset(&a->in, 0, sizeof a->in);
}

/* built once at startup so P_* overrides apply */
static char env_buf[16][CFG_STR * 2];
static const char *minarch_env[17];

static void build_child_env(void)
{
	int n = 0;
	const char *fixed[] = {
		"PLATFORM=tg5040",
		"DEVICE=brick",
		"SDCARD_PATH=/mnt/SDCARD",
		"BIOS_PATH=/mnt/SDCARD/Bios",
		"CHEATS_PATH=/mnt/SDCARD/Cheats",
		"SAVES_PATH=/mnt/SDCARD/Saves",
	};
	for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
		minarch_env[n++] = fixed[i];
	snprintf(env_buf[0], sizeof env_buf[0], "ROMS_PATH=%s", P_ROMS);
	snprintf(env_buf[1], sizeof env_buf[1], "SYSTEM_PATH=%s", P_ROOT);
	snprintf(env_buf[2], sizeof env_buf[2], "CORES_PATH=%s/cores", P_ROOT);
	snprintf(env_buf[3], sizeof env_buf[3], "USERDATA_PATH=%s", P_USERDATA);
	snprintf(env_buf[4], sizeof env_buf[4], "SHARED_USERDATA_PATH=%s", P_SHARED);
	snprintf(env_buf[5], sizeof env_buf[5], "LOGS_PATH=%s/logs", P_USERDATA);
	snprintf(env_buf[6], sizeof env_buf[6], "HOME=%s", P_USERDATA);
	snprintf(env_buf[7], sizeof env_buf[7], "LD_LIBRARY_PATH=%s/lib:/usr/trimui/lib", P_ROOT);
	for (int i = 0; i <= 7; i++)
		minarch_env[n++] = env_buf[i];
	minarch_env[n] = NULL;
}

/* True if the Muse daemon is actually PLAYING (not paused/stopped). Only then do
 * we keep the music and mute the game; a paused or stopped daemon lets the game
 * take audio normally. Status fields: track pos dur paused active ... */
static bool music_is_active(void)
{
	FILE *f = fopen("/tmp/musicd.status", "r");
	if (!f) return false;
	int track = 0, paused = 0, active = 0;
	double pos = 0, dur = 0;
	int got = fscanf(f, "%d %lf %lf %d %d", &track, &pos, &dur, &paused, &active);
	fclose(f);
	return got >= 5 && active != 0 && paused == 0;
}

static void launch_game(app *a, int game_idx)
{
	card_cfg *sys = &a->sys.cards[a->sys_cursor];
	game_entry *g = &a->games[game_idx];

	char core_path[CFG_STR * 2];
	snprintf(core_path, sizeof core_path, "%s/cores/%s_libretro.so", P_ROOT, sys->core);
	char rom_path[CFG_STR * 3];
	snprintf(rom_path, sizeof rom_path, "%s/%s/%s", P_ROMS, sys->target, g->file);

	/* Always request the auto-resume slot, exactly like NextUI does:
	 * minarch loads slot 9 if that state exists and silently starts
	 * fresh if it doesn't. */
	FILE *f = fopen("/tmp/resume_slot.txt", "w");
	if (f) { fputs("9", f); fclose(f); }

	char minarch[CFG_STR * 2];
	snprintf(minarch, sizeof minarch, "%s/minarch.elf", P_ROOT);
	char *argv[] = { minarch, core_path, rom_path, NULL };

	const char *env2[20];
	int n = 0;
	for (; minarch_env[n]; n++) env2[n] = minarch_env[n];
	if (music_is_active()) {
		/* Music is playing -- keep it. Run the core with a null SDL audio driver
		 * so it never opens the ALSA device (the Muse daemon keeps it): the game
		 * is silent, the music plays straight through. Volume/brightness keys
		 * in-game still work and adjust the codec, so they control the music. */
		env2[n++] = "SDL_AUDIODRIVER=dummy";
	} else {
		system("killall -q muse 2> /dev/null"); /* no music: let the core own audio */
		/* Route the game to a BT headset when one's connected and the switch is on
		 * BT: the boot loop publishes the bluealsa device string to eros_bt_route
		 * (empty/absent otherwise). Neither SDL nor in-process ALSA can drive
		 * bluealsa from the game (both hang), so we run a standalone `aplay`
		 * forwarder that owns the sink and have minarch pipe raw 48k S16 stereo PCM
		 * to a FIFO; aplay plays it to the headset. Absent -> speaker (SDL). */
		FILE *rf = fopen("/tmp/eros_bt_route", "r");
		if (rf) {
			char dev[128] = "";
			if (fgets(dev, sizeof dev, rf)) {
				dev[strcspn(dev, "\r\n")] = '\0';
				if (dev[0]) {
					char cmd[360];
					snprintf(cmd, sizeof cmd,
					    "killall -q aplay 2>/dev/null; rm -f %s; mkfifo %s; "
					    "(sh %s/btout.sh '%s' %s &)",
					    BT_FIFO, BT_FIFO, P_ROOT, dev, BT_FIFO);
					system(cmd);
					env2[n++] = "EROS_BT_SINK=" BT_FIFO;
				}
			}
			fclose(rf);
		}
	}
	env2[n] = NULL;
	run_child_with_video_handoff(a, argv, env2, P_USERDATA);
	/* textures were torn down for the handoff; rewarm the window so the
	 * return to Cover Flow scrolls smoothly */
	if (a->running && a->screen == SCREEN_GAMES) prime_game_window(a);
}

static void launch_app(app *a, int card_idx)
{
	card_cfg *c = &a->sys.cards[card_idx];
	if (access(c->target, F_OK) != 0) {
		fprintf(stderr, "app card '%s': %s missing\n", c->name, c->target);
		return;
	}
	char *argv[] = { "/bin/sh", "-c", c->target, NULL };
	run_child_with_video_handoff(a, argv, minarch_env, P_ROOT);
}

/* ---- screens ---- */

static void draw_background(SDL_Renderer *r)
{
	SDL_SetRenderDrawColor(r, 10, 11, 16, 255);
	SDL_RenderClear(r);
}

/* True at most once every few seconds: battery at/below the low mark and not
 * charging. Throttled so we aren't stat-ing sysfs every frame. */
static bool battery_low(void)
{
	static Uint32 next_check;
	static bool low;
	Uint32 now = SDL_GetTicks();
	if (next_check == 0 || now >= next_check) {
		next_check = now + 5000;
		int pct;
		bool charging;
		low = plat_battery(&pct, &charging) && !charging && pct <= BATT_LOW_PCT;
	}
	return low;
}

/* The one allowed piece of chrome: a small red disc, top-right, when the
 * battery is low. Filled with horizontal spans (no circle primitive in SDL). */
static void draw_low_battery_dot(SDL_Renderer *r)
{
	int cx = EROS_SCREEN_W - 34, cy = 32, rad = 11;
	SDL_SetRenderDrawColor(r, 224, 52, 52, 255);
	for (int dy = -rad; dy <= rad; dy++) {
		int dx = (int)(sqrt((double)(rad * rad - dy * dy)) + 0.5);
		SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
	}
}

static void update_systems(app *a)
{
	in_state *in = &a->in;
	int n = a->sys.count;
	if (in_repeat(in, IN_LEFT)) a->sys_cursor = (a->sys_cursor - 1 + n) % n;
	if (in_repeat(in, IN_RIGHT)) a->sys_cursor = (a->sys_cursor + 1) % n;
	if (in->pressed[IN_ACCEPT]) {
		card_cfg *c = &a->sys.cards[a->sys_cursor];
		if (c->kind == CARD_APP) {
			launch_app(a, a->sys_cursor);
			prime_sys_window(a); /* app tore textures down; rewarm before scroll */
		} else if (load_games(a, c->target)) {
			a->game_cursor = 0;
			cf_reset(&a->cf_games, 0);
			a->screen = SCREEN_GAMES;
			prime_game_window(a);
		} else {
			fprintf(stderr, "no games in %s\n", c->target);
		}
	}
	cf_set_cursor(&a->cf_sys, a->sys_cursor, n);
}

static void update_games(app *a)
{
	in_state *in = &a->in;
	int n = a->game_count;
	if (in_repeat(in, IN_LEFT)) a->game_cursor = (a->game_cursor - 1 + n) % n;
	if (in_repeat(in, IN_RIGHT)) a->game_cursor = (a->game_cursor + 1) % n;
	if (in->pressed[IN_BACK]) {
		a->screen = SCREEN_SYSTEMS;
		free_games(a);
		prime_sys_window(a); /* sys icons may have been freed by a game launch */
		return;
	}
	if (in->pressed[IN_ACCEPT]) launch_game(a, a->game_cursor);
	if (a->screen == SCREEN_GAMES)
		cf_set_cursor(&a->cf_games, a->game_cursor, n);
}

static void render(app *a)
{
	SDL_Renderer *r = plat_renderer();
	draw_background(r);

	if (a->screen == SCREEN_SYSTEMS) {
		cf_draw(&a->cf_sys, r, EROS_SCREEN_W, EROS_SCREEN_H, a->sys.count,
		        sys_get_tex, a, &CF_LAYOUT_SYSTEMS);
	} else {
		cf_draw(&a->cf_games, r, EROS_SCREEN_W, EROS_SCREEN_H, a->game_count,
		        game_get_tex, a, &CF_LAYOUT_GAMES);
		evict_far_game_tex(a);
	}

	if (battery_low()) draw_low_battery_dot(r);

	if (want_shot) { want_shot = 0; save_shot(r); }
	plat_draw_osd(r);   /* volume/brightness line, on top of everything */
	SDL_RenderPresent(r);
}

int main(int argc, char *argv[])
{
	(void)argc; (void)argv;
	signal(SIGUSR1, on_sigusr1);
	signal(SIGTERM, on_sigterm);
	signal(SIGINT, on_sigterm);
	script_init();

	app a = {0};
	paths_init();
	build_child_env();
	char cfgpath[CFG_STR * 2];
	snprintf(cfgpath, sizeof cfgpath, "%s/systems.cfg", P_ROOT);
	if (!cfg_load_systems(cfgpath, &a.sys)) {
		fprintf(stderr, "no usable %s\n", cfgpath);
		return 1;
	}
	snprintf(cfgpath, sizeof cfgpath, "%s/eros.cfg", P_ROOT);
	cfg_load_eros(cfgpath, &a.cfg);

	if (!plat_video_init()) return 1;
	if (!plat_input_init()) return 1;
	IMG_Init(IMG_INIT_PNG);
	if (TTF_Init() != 0) fprintf(stderr, "ttf: %s\n", TTF_GetError());
	a.font_big = TTF_OpenFont(P_FONT, 34);
	btpair_init(cfgpath);   /* cfgpath still points at eros.cfg */

	plat_leds_off();
	plat_settings_init();
	if (a.cfg.volume >= 0) plat_volume_set_pct(a.cfg.volume);
	/* Always (re)apply brightness after InitSettings -- it can leave a stale/dim
	 * stored value, which made the launcher darker than the boot animation.
	 * Default to level 8 (~raw 160), matching launch.sh + the boot splash. */
	plat_brightness_set(a.cfg.brightness >= 0 ? a.cfg.brightness : 8);

	a.sys_cursor = 0;
	if (a.cfg.startup_system[0]) {
		for (int i = 0; i < a.sys.count; i++) {
			if (strcasecmp(a.sys.cards[i].name, a.cfg.startup_system) == 0) {
				a.sys_cursor = i;
				break;
			}
		}
	}
	cf_reset(&a.cf_sys, a.sys_cursor);
	prime_sys_window(&a); /* warm the icon textures before the first scroll */
	a.running = true;

	/* swallow boot-time input noise (replayed wake presses, device-init
	 * bursts) before honoring anything */
	Uint32 grace_until = SDL_GetTicks() + 500;

	while (a.running) {
		plat_input_poll(&a.in);
		if (SDL_GetTicks() < grace_until)
			memset(a.in.pressed, 0, sizeof a.in.pressed);
		script_step(&a.in);
		if (a.in.quit_requested || want_quit) break;
		if (a.in.pressed[IN_POWER]) {
			plat_request_poweroff();
			show_poweroff_message(&a);
			break;
		}
		/* volume + brightness work on every screen, with hold-to-repeat */
		if (in_repeat(&a.in, IN_VOLUP)) plat_volume_nudge(+1);
		if (in_repeat(&a.in, IN_VOLDN)) plat_volume_nudge(-1);
		if (in_repeat(&a.in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
		if (in_repeat(&a.in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

		/* The BT pair/connect screen takes over when the switch is on BT but no
		 * sink is connected; otherwise Cover Flow runs as usual. */
		btpair_poll();
		btpair_tick();
		if (btpair_active()) {
			btpair_update(&a.in);
			btpair_render(plat_renderer());
			continue;
		}

		if (a.screen == SCREEN_SYSTEMS) update_systems(&a);
		else update_games(&a);
		if (!a.running) break;

		render(&a);
	}

	free_games(&a);
	free_all_textures(&a);
	if (a.font_big) TTF_CloseFont(a.font_big);
	TTF_Quit();
	IMG_Quit();
	plat_input_quit();
	plat_video_quit();
	SDL_Quit();
	return 0;
}
