/* Muse -- the EROS music player. An app card that slots into the Cover Flow
 * alongside the systems: browse albums as a Cover Flow of cover art, drill into
 * a track list, and a focused now-playing screen. Reuses the EROS platform +
 * coverflow so it looks and drives exactly like the launcher. Audio runs in a
 * background daemon (player.c) over raw ALSA on the device / SDL on the dev
 * build (see audio.c). */
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "eros_power_png.h"
#include "library.h"
#include "player.h"
#include "../../src/coverflow.h"
#include "../../src/eros_text.h"
#include "../../src/platform.h"

#define COVER_KEEP 8    /* large libraries: evict cover textures farther than this */
#define COVER_PRIME_ALL 48 /* libraries this size or smaller: load all covers once and keep them */
#define LOWRES_MAX 360  /* now-playing art whose source is smaller than this (drawn at 440) is tagged "low-res" */

typedef enum { SCREEN_ALBUMS, SCREEN_TRACKS, SCREEN_NOWPLAYING } screen_id;

typedef struct {
	mus_library lib;
	TTF_Font *font;       /* track/album names */
	TTF_Font *font_small; /* secondary text */
	TTF_Font *font_title; /* now-playing track title */
	TTF_Font *font_album; /* now-playing album name */
	TTF_Font *font_meta;  /* now-playing times + play state */

	SDL_Texture **cover;  /* per-album cover texture cache */
	int *cover_w, *cover_h;

	/* background cover decode: a worker decodes upcoming covers to surfaces off
	 * the render thread; the main thread uploads ready surfaces to textures. */
	SDL_Surface  **surf;    /* worker-decoded, not-yet-uploaded surface per album */
	unsigned char *cstate;  /* 0 idle, 1 in-flight, 2 surface ready */
	int           *reqq, req_head, req_tail, req_cap, req_len;  /* request ring */
	pthread_mutex_t clk;
	pthread_cond_t  ccv;
	pthread_t       cthr;
	bool            cthr_on;
	volatile bool   cquit;
	SDL_Texture    *placeholder;  /* shown while a cover streams in (shared) */
	int             ph_w, ph_h;

	/* per-line marquee clocks: 0 = album name (cover flow / track header),
	 * 1 = now-playing title+subtitle, 2 = selected track row */
	Uint32 mq_since[3];
	int    mq_key[3];

	screen_id screen;
	int album_cursor;
	int track_cursor;     /* selection in the track list */
	coverflow cf;

	int play_album;       /* album the current queue came from (-1 = none) */
	int play_track;       /* index within that album */
	player_state ps;      /* last snapshot of the background player's state */

	in_state in;
	bool running;
} app;

static volatile sig_atomic_t want_quit;
static volatile sig_atomic_t want_shot;
static void on_term(int sig) { (void)sig; want_quit = 1; }
static void on_usr1(int sig) { (void)sig; want_shot = 1; }

/* dev-only screenshot to /tmp on SIGUSR1 (native verification) */
static void save_shot(SDL_Renderer *r)
{
	static int no;
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, EROS_SCREEN_W, EROS_SCREEN_H,
	                                                32, SDL_PIXELFORMAT_RGBA32);
	if (!s) return;
	if (SDL_RenderReadPixels(r, NULL, s->format->format, s->pixels, s->pitch) == 0) {
		char p[64];
		snprintf(p, sizeof p, "/tmp/eros_music_%d.bmp", no++);
		SDL_SaveBMP(s, p);
	}
	SDL_FreeSurface(s);
}

/* EROS_SCRIPT="right,a,down,a,shot,quit" -- one action every few frames, to
 * drive the UI without hands (dev only). */
static char script_buf[512];
static char *script_next;
static void script_init(void)
{
	const char *s = getenv("EROS_SCRIPT");
	if (s) { snprintf(script_buf, sizeof script_buf, "%s", s); script_next = script_buf; }
}
static void script_step(in_state *in)
{
	static int cd;
	if (!script_next || !*script_next) return;
	if (++cd < 18) return;
	cd = 0;
	char *tok = script_next, *comma = strchr(tok, ',');
	if (comma) { *comma = '\0'; script_next = comma + 1; }
	else script_next = tok + strlen(tok);
	if (!strcmp(tok, "left")) in->pressed[IN_LEFT] = true;
	else if (!strcmp(tok, "right")) in->pressed[IN_RIGHT] = true;
	else if (!strcmp(tok, "up")) in->pressed[IN_UP] = true;
	else if (!strcmp(tok, "down")) in->pressed[IN_DOWN] = true;
	else if (!strcmp(tok, "a")) in->pressed[IN_ACCEPT] = true;
	else if (!strcmp(tok, "b")) in->pressed[IN_BACK] = true;
	else if (!strcmp(tok, "shot")) want_shot = 1;
	else if (!strcmp(tok, "quit")) in->quit_requested = true;
}

/* square album Cover Flow: big cover art whose reflection reaches the bottom
 * of the screen (like the games row). For square art the reflection baseline
 * lands at center_y + (size/2)*reflect of screen height, so center_y 0.46 +
 * size 0.66 -> reflect ~1.64 puts the reflection floor right at the bottom. */
static const cf_layout CF_ALBUMS = {
	.size = 0.66f, .aspect = 1.0f, .step = 0.82f, .side_scale = 0.60f,
	.center_y = 0.46f, .tilt = 0.80f, .reflect = 1.64f,
	.side_alpha = 150, .strips = 16,
};

/* ---- text ---- */

static void draw_text(SDL_Renderer *r, TTF_Font *font, const char *s,
                      int x, int y, SDL_Color col, int center_x, int max_w)
{
	if (!font || !s || !*s) return;
	SDL_Surface *surf = max_w > 0
	    ? TTF_RenderUTF8_Blended_Wrapped(font, s, col, max_w)
	    : TTF_RenderUTF8_Blended(font, s, col);
	if (!surf) return;
	SDL_Texture *t = SDL_CreateTextureFromSurface(r, surf);
	SDL_Rect dst = { center_x ? x - surf->w / 2 : x, y, surf->w, surf->h };
	SDL_RenderCopy(r, t, NULL, &dst);
	SDL_DestroyTexture(t);
	SDL_FreeSurface(surf);
}

/* ---- single-line text: truncate, or slow ping-pong marquee when it overflows.
 * Long names never wrap to a second line -- they either get an ellipsis or gently
 * scroll their focused line back and forth. ---- */

/* animation clock (ms) for a marquee slot; resets when `key` changes so a new
 * selection/track restarts the scroll from the beginning */
static Uint32 marquee_clock(app *a, int slot, int key)
{
	if (a->mq_key[slot] != key) { a->mq_key[slot] = key; a->mq_since[slot] = SDL_GetTicks(); }
	return SDL_GetTicks() - a->mq_since[slot];
}

/* Draw `s` on one line within [x, x+box_w]. Fits -> drawn (centered or left).
 * Overflows -> a slow ping-pong scroll, clipped to the box (never wraps). */
static void draw_line_marquee(SDL_Renderer *r, TTF_Font *font, const char *s,
                              int x, int y, int box_w, SDL_Color col,
                              int centered, Uint32 t)
{
	if (!font || !s || !*s) return;
	SDL_Surface *surf = TTF_RenderUTF8_Blended(font, s, col);
	if (!surf) return;
	int tw = surf->w, th = surf->h;
	SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
	SDL_FreeSurface(surf);
	if (!tex) return;
	if (tw <= box_w) {
		SDL_Rect dst = { centered ? x + (box_w - tw) / 2 : x, y, tw, th };
		SDL_RenderCopy(r, tex, NULL, &dst);
	} else {
		const Uint32 PAUSE = 1400;   /* hold at each end (ms) */
		const double SPEED = 42.0;   /* px/sec -- slow + gentle */
		int over = tw - box_w;
		Uint32 scroll = (Uint32)(over / SPEED * 1000.0);
		if (scroll < 1) scroll = 1;
		Uint32 cycle = 2 * PAUSE + 2 * scroll, p = t % cycle;
		double off;
		if (p < PAUSE) off = 0;
		else if (p < PAUSE + scroll) off = over * (double)(p - PAUSE) / scroll;
		else if (p < 2 * PAUSE + scroll) off = over;
		else off = over * (1.0 - (double)(p - 2 * PAUSE - scroll) / scroll);
		SDL_Rect clip = { x, y, box_w, th };
		SDL_RenderSetClipRect(r, &clip);
		SDL_Rect dst = { x - (int)off, y, tw, th };
		SDL_RenderCopy(r, tex, NULL, &dst);
		SDL_RenderSetClipRect(r, NULL);
	}
	SDL_DestroyTexture(tex);
}

/* Draw `s` on one line; if it overflows [x, x+box_w], truncate with an ellipsis. */
static void draw_line_trunc(SDL_Renderer *r, TTF_Font *font, const char *s,
                            int x, int y, int box_w, SDL_Color col, int centered)
{
	if (!font || !s || !*s) return;
	int tw = 0;
	TTF_SizeUTF8(font, s, &tw, NULL);
	char buf[MUS_STR * 2 + 8];
	const char *draw = s;
	if (tw > box_w) {
		size_t n = strlen(s);
		while (n > 0) {
			do { n--; } while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80); /* whole UTF-8 chars */
			snprintf(buf, sizeof buf, "%.*s…", (int)n, s);
			int ww = 0; TTF_SizeUTF8(font, buf, &ww, NULL);
			if (ww <= box_w) break;
		}
		draw = buf;
		centered = 0;   /* truncated text reads best left-anchored */
	}
	SDL_Surface *surf = TTF_RenderUTF8_Blended(font, draw, col);
	if (!surf) return;
	int dw = surf->w, dh = surf->h;
	SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
	SDL_FreeSurface(surf);
	if (!tex) return;
	SDL_Rect dst = { centered ? x + (box_w - dw) / 2 : x, y, dw, dh };
	SDL_RenderCopy(r, tex, NULL, &dst);
	SDL_DestroyTexture(tex);
}

/* ---- play-mode glyphs (drawn as primitives -- the font has no such symbols) ---- */

static void fill_tri(SDL_Renderer *r, float ax, float ay, float bx, float by,
                     float cx, float cy, SDL_Color col)
{
	SDL_Vertex v[3] = {
		{ { ax, ay }, col, { 0, 0 } }, { { bx, by }, col, { 0, 0 } }, { { cx, cy }, col, { 0, 0 } },
	};
	SDL_RenderGeometry(r, NULL, v, 3, NULL, 0);
}

/* a `w`-thick line from (x1,y1) to (x2,y2), as a quad of two triangles */
static void thick_line(SDL_Renderer *r, float x1, float y1, float x2, float y2,
                       float w, SDL_Color col)
{
	float dx = x2 - x1, dy = y2 - y1, len = sqrtf(dx * dx + dy * dy);
	if (len < 0.01f) return;
	float nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
	fill_tri(r, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x2 - nx, y2 - ny, col);
	fill_tri(r, x1 + nx, y1 + ny, x2 - nx, y2 - ny, x2 + nx, y2 + ny, col);
}

/* a chevron arrowhead at tip (tx,ty) pointing along (dirx,diry) */
static void arrowhead(SDL_Renderer *r, float tx, float ty, float dirx, float diry,
                      float sz, float w, SDL_Color col)
{
	float len = sqrtf(dirx * dirx + diry * diry);
	if (len < 0.01f) return;
	dirx /= len; diry /= len;
	float px = -diry, py = dirx;
	thick_line(r, tx, ty, tx - (dirx - px) * sz, ty - (diry - py) * sz, w, col);
	thick_line(r, tx, ty, tx - (dirx + px) * sz, ty - (diry + py) * sz, w, col);
}

/* repeat: a rectangular loop with two circulating arrowheads. The caller draws an
 * "all" / "1" label beside it to tell repeat-all from repeat-one. */
static void icon_repeat(SDL_Renderer *r, float cx, float cy, float s, float w, SDL_Color col)
{
	float hw = s, hh = s * 0.72f;
	float L = cx - hw, R = cx + hw, T = cy - hh, B = cy + hh;
	thick_line(r, L, T, R, T, w, col);   /* top */
	thick_line(r, R, T, R, B, w, col);   /* right */
	thick_line(r, R, B, L, B, w, col);   /* bottom */
	thick_line(r, L, B, L, T, w, col);   /* left */
	arrowhead(r, cx + s * 0.15f, T, 1, 0, s * 0.42f, w, col);   /* top edge, flowing right */
	arrowhead(r, cx - s * 0.15f, B, -1, 0, s * 0.42f, w, col);  /* bottom edge, flowing left */
}

/* shuffle: two crossing paths with arrowheads on their right ends */
static void icon_shuffle(SDL_Renderer *r, float cx, float cy, float s, float w, SDL_Color col)
{
	float hw = s, hh = s * 0.72f;
	thick_line(r, cx - hw, cy - hh, cx + hw, cy + hh, w, col);
	thick_line(r, cx - hw, cy + hh, cx + hw, cy - hh, w, col);
	arrowhead(r, cx + hw, cy + hh, 1, 1, s * 0.42f, w, col);
	arrowhead(r, cx + hw, cy - hh, 1, -1, s * 0.42f, w, col);
}

static TTF_Font *open_font(int size);   /* fwd: the MUSE wordmark wants a big font */

/* branded cover for the loose "Singles" bucket and Playlists/ collections: the
 * EROS-red power symbol over a wordmark ("MUSE" for singles, the folder name for
 * a collection). Drawn off-screen with a software renderer, then handed to the
 * main renderer. */
static SDL_Texture *make_branded_cover(SDL_Renderer *r, const char *label, int *w, int *h)
{
	int s = 512;
	SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, s, s, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!surf) return NULL;
	SDL_Renderer *sr = SDL_CreateSoftwareRenderer(surf);
	if (sr) {
		SDL_SetRenderDrawColor(sr, 30, 32, 40, 255);
		SDL_RenderClear(sr);
		SDL_Color fg = { 222, 222, 230, 255 };
		/* the power symbol, clipped from the EROS logo (embedded PNG) */
		SDL_Texture *pw = IMG_LoadTexture_RW(sr, SDL_RWFromConstMem(eros_power_png, eros_power_png_len), 1);
		if (pw) {
			float D = s * 0.46f;
			SDL_Rect pd = { (int)(s / 2 - D / 2), (int)(s * 0.40f - D / 2), (int)D, (int)D };
			SDL_RenderCopy(sr, pw, NULL, &pd);
			SDL_DestroyTexture(pw);
		}
		TTF_Font *big = open_font(150);
		if (big && label && label[0]) {
			SDL_Surface *t = TTF_RenderUTF8_Blended(big, label, fg);
			if (t) {
				SDL_Texture *tx = SDL_CreateTextureFromSurface(sr, t);
				if (tx) {
					float dw = s * 0.72f;
					if (dw > t->w * 2.2f) dw = t->w * 2.2f;   /* don't over-scale short labels */
					float dh = dw * t->h / t->w;
					SDL_Rect d = { (int)((s - dw) / 2), (int)(s * 0.66f), (int)dw, (int)dh };
					SDL_RenderCopy(sr, tx, NULL, &d);
					SDL_DestroyTexture(tx);
				}
				SDL_FreeSurface(t);
			}
		}
		if (big) TTF_CloseFont(big);
		SDL_RenderPresent(sr);
		SDL_DestroyRenderer(sr);
	}
	SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
	if (tex) { *w = s; *h = s; }
	SDL_FreeSurface(surf);
	return tex;
}

/* ---- cover textures ---- */

static SDL_Texture *make_fallback_cover(SDL_Renderer *r, TTF_Font *font,
                                        const char *album, int *w, int *h)
{
	int s = 512;
	SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, s, s, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!surf) return NULL;
	SDL_FillRect(surf, NULL, SDL_MapRGBA(surf->format, 30, 32, 40, 255));
	/* no art -> the album name, centered (the artist is shown above the cover) */
	if (font && album && album[0]) {
		SDL_Color col = { 210, 210, 216, 255 };
		SDL_Surface *txt = TTF_RenderUTF8_Blended_Wrapped(font, album, col, s - 80);
		if (txt) {
			SDL_Rect dst = { (s - txt->w) / 2, (s - txt->h) / 2, txt->w, txt->h };
			SDL_BlitSurface(txt, NULL, surf, &dst);
			SDL_FreeSurface(txt);
		}
	}
	SDL_Texture *t = SDL_CreateTextureFromSurface(r, surf);
	if (t) { *w = s; *h = s; }
	SDL_FreeSurface(surf);
	return t;
}

/* a plain dark square shown for a frame or two while the worker decodes the real
 * cover (only ever seen on large libraries during a fast scroll) */
static SDL_Texture *make_placeholder(SDL_Renderer *r, int *w, int *h)
{
	int s = 512;
	SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, s, s, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!surf) return NULL;
	SDL_FillRect(surf, NULL, SDL_MapRGBA(surf->format, 30, 32, 40, 255));
	SDL_Texture *t = SDL_CreateTextureFromSurface(r, surf);
	if (t) { *w = s; *h = s; }
	SDL_FreeSurface(surf);
	return t;
}

/* Decode album i's cover (file or embedded bytes) to a surface. Safe off the
 * render thread: it never touches the renderer, only the read-only album data. */
static SDL_Surface *cover_decode_surface(app *a, int i)
{
	mus_album *al = &a->lib.albums[i];
	if (al->cover[0]) return IMG_Load(al->cover);
	if (al->art) {
		SDL_RWops *rw = SDL_RWFromConstMem(al->art, al->art_len);
		return rw ? IMG_Load_RW(rw, 1) : NULL;
	}
	return NULL;
}

static int cover_worker_pop(app *a)
{
	pthread_mutex_lock(&a->clk);
	while (a->req_len == 0 && !a->cquit) pthread_cond_wait(&a->ccv, &a->clk);
	int i = -1;
	if (a->req_len > 0) {
		i = a->reqq[a->req_head];
		a->req_head = (a->req_head + 1) % a->req_cap;
		a->req_len--;
	}
	pthread_mutex_unlock(&a->clk);
	return i;
}

static void *cover_worker(void *ctx)
{
	app *a = ctx;
	for (;;) {
		int i = cover_worker_pop(a);
		if (i < 0) break;   /* woken to quit */
		SDL_Surface *s = cover_decode_surface(a, i);
		pthread_mutex_lock(&a->clk);
		if (s && a->cstate[i] == 1) { a->surf[i] = s; a->cstate[i] = 2; }
		else { if (s) SDL_FreeSurface(s); if (a->cstate[i] == 1) a->cstate[i] = 0; }
		pthread_mutex_unlock(&a->clk);
	}
	return NULL;
}

/* Ask the worker to decode album i (no-op if we already have it or it's queued). */
static void cover_request(app *a, int i)
{
	if (a->cover[i] || !a->cthr_on) return;
	pthread_mutex_lock(&a->clk);
	if (a->cstate[i] == 0 && a->req_len < a->req_cap) {
		a->cstate[i] = 1;
		a->reqq[a->req_tail] = i;
		a->req_tail = (a->req_tail + 1) % a->req_cap;
		a->req_len++;
		pthread_cond_signal(&a->ccv);
	}
	pthread_mutex_unlock(&a->clk);
}

/* If the worker has a surface ready for album i, upload it to a texture. */
static void cover_drain(app *a, int i)
{
	if (a->cover[i] || !a->cthr_on) return;
	pthread_mutex_lock(&a->clk);
	SDL_Surface *s = NULL;
	if (a->cstate[i] == 2) { s = a->surf[i]; a->surf[i] = NULL; a->cstate[i] = 0; }
	pthread_mutex_unlock(&a->clk);
	if (!s) return;
	a->cover[i] = SDL_CreateTextureFromSurface(plat_renderer(), s);
	if (a->cover[i]) { a->cover_w[i] = s->w; a->cover_h[i] = s->h; }
	SDL_FreeSurface(s);
}

/* Decode + cache album i's cover on this thread, unconditionally. Used for the
 * focused card and for priming small libraries. */
static void cover_load_sync(app *a, int i)
{
	if (a->cover[i]) return;
	mus_album *al = &a->lib.albums[i];
	if (al->branded) {   /* power+wordmark cover, generated once */
		a->cover[i] = make_branded_cover(plat_renderer(), al->is_singles ? "MUSE" : al->name,
		                                 &a->cover_w[i], &a->cover_h[i]);
		if (a->cover[i]) return;
	}
	SDL_Surface *s = cover_decode_surface(a, i);
	if (s) {
		a->cover[i] = SDL_CreateTextureFromSurface(plat_renderer(), s);
		if (a->cover[i]) { a->cover_w[i] = s->w; a->cover_h[i] = s->h; }
		SDL_FreeSurface(s);
	}
	if (!a->cover[i])
		a->cover[i] = make_fallback_cover(plat_renderer(), a->font, a->lib.albums[i].name,
		                                  &a->cover_w[i], &a->cover_h[i]);
}

static SDL_Texture *cover_get(void *ctx, int i, int *w, int *h)
{
	app *a = ctx;
	cover_drain(a, i);   /* pick up anything the worker finished */
	if (!a->cover[i]) {
		mus_album *al = &a->lib.albums[i];
		bool has_art = al->cover[0] || al->art;
		if (al->branded) {   /* power+wordmark cover (Singles / Playlists) */
			cover_load_sync(a, i);
		} else if (!has_art) {   /* no art at all -> a named fallback, cached for good */
			a->cover[i] = make_fallback_cover(plat_renderer(), a->font, al->name,
			                                  &a->cover_w[i], &a->cover_h[i]);
		} else if (i == a->album_cursor || !a->cthr_on) {
			cover_load_sync(a, i);   /* focused card is never a placeholder */
		} else {                     /* stream it in; show the placeholder meanwhile */
			cover_request(a, i);
			*w = a->ph_w; *h = a->ph_h;
			return a->placeholder;
		}
	}
	*w = a->cover_w[i];
	*h = a->cover_h[i];
	return a->cover[i];
}

static void cover_evict_far(app *a)
{
	int n = a->lib.count;
	if (n <= COVER_PRIME_ALL) return; /* small library: keep every cover cached */
	for (int i = 0; i < n; i++) {
		int d = abs(i - a->album_cursor);
		if (n >= CF_WINDOW) d = d > n / 2 ? n - d : d;
		if (d <= COVER_KEEP) continue;
		if (a->cover[i]) { SDL_DestroyTexture(a->cover[i]); a->cover[i] = NULL; }
		pthread_mutex_lock(&a->clk);
		if (a->surf[i]) { SDL_FreeSurface(a->surf[i]); a->surf[i] = NULL; }
		if (a->cstate[i] != 1) a->cstate[i] = 0;   /* let an in-flight decode finish + discard */
		pthread_mutex_unlock(&a->clk);
	}
}

/* Warm the covers just off either edge of the cursor so a fast scroll finds them
 * already decoded (no-op for small libraries -- every cover is already cached). */
static void cover_prefetch(app *a)
{
	int n = a->lib.count;
	if (n <= COVER_PRIME_ALL) return;
	for (int k = -(COVER_KEEP); k <= COVER_KEEP; k++) {
		int i = a->album_cursor + k;
		if (n >= 2) { i %= n; if (i < 0) i += n; }
		else if (i < 0 || i >= n) continue;
		cover_request(a, i);
	}
}

static void cover_prime(app *a)
{
	int n = a->lib.count;
	/* small library: decode every cover once, up front (masked by the app-open
	 * transition), so scrolling never has to load/evict again */
	if (n <= COVER_PRIME_ALL) {
		for (int i = 0; i < n; i++) cover_load_sync(a, i);
		return;
	}
	cover_load_sync(a, a->album_cursor);   /* focused card sharp immediately */
	cover_prefetch(a);                     /* the rest of the window streams in */
}

static void cover_decode_start(app *a)
{
	int n = a->lib.count;
	if (n <= COVER_PRIME_ALL) return;   /* small libraries never need the worker */
	a->surf = calloc(n, sizeof *a->surf);
	a->cstate = calloc(n, 1);
	a->req_cap = n;
	a->reqq = calloc(n, sizeof *a->reqq);
	if (!a->surf || !a->cstate || !a->reqq) return;
	pthread_mutex_init(&a->clk, NULL);
	pthread_cond_init(&a->ccv, NULL);
	if (pthread_create(&a->cthr, NULL, cover_worker, a) == 0) a->cthr_on = true;
}

static void cover_decode_stop(app *a)
{
	if (a->cthr_on) {
		pthread_mutex_lock(&a->clk);
		a->cquit = true;
		pthread_cond_signal(&a->ccv);
		pthread_mutex_unlock(&a->clk);
		pthread_join(a->cthr, NULL);
		a->cthr_on = false;
		pthread_mutex_destroy(&a->clk);
		pthread_cond_destroy(&a->ccv);
	}
	if (a->surf) { for (int i = 0; i < a->lib.count; i++) if (a->surf[i]) SDL_FreeSurface(a->surf[i]); free(a->surf); a->surf = NULL; }
	free(a->cstate); a->cstate = NULL;
	free(a->reqq); a->reqq = NULL;
}

/* ---- playback ---- */

/* Playback all runs in the daemon (see player.c) so it survives the UI closing.
 * These just send commands; a->play_track resyncs from the daemon's status. */
static bool play_index(app *a, int album, int track)
{
	mus_album *al = &a->lib.albums[album];
	if (track < 0 || track >= al->track_count) return false;
	player_play(al->dir, track, 0.0);
	a->play_album = album;
	a->play_track = track;
	return true;
}

static void play_step(app *a, int delta)
{
	if (a->play_album < 0) return;
	if (delta < 0) player_prev();
	else player_next();
}

/* ---- rendering ---- */

static void draw_bg(SDL_Renderer *r)
{
	SDL_SetRenderDrawColor(r, 10, 11, 16, 255);
	SDL_RenderClear(r);
}

static void render_albums(app *a)
{
	SDL_Renderer *r = plat_renderer();
	draw_bg(r);
	cf_draw(&a->cf, r, EROS_SCREEN_W, EROS_SCREEN_H, a->lib.count, cover_get, a, &CF_ALBUMS);
	cover_prefetch(a);   /* keep the worker a step ahead of the scroll */
	cover_evict_far(a);
	/* name at the top: the bigger cover + full-height reflection now own the
	 * lower screen, so the label lives above the card */
	/* the albums are sorted + grouped by artist, so the artist is the browse key
	 * shown above the covers (the cover art itself carries the album) */
	SDL_Color white = { 235, 235, 240, 255 };
	mus_album *al = &a->lib.albums[a->album_cursor];
	draw_line_marquee(r, a->font, al->artist[0] ? al->artist : al->name,
	                  60, 30, EROS_SCREEN_W - 120, white, 1,
	                  marquee_clock(a, 0, a->album_cursor));
}

static void render_tracks(app *a)
{
	SDL_Renderer *r = plat_renderer();
	draw_bg(r);
	mus_album *al = &a->lib.albums[a->album_cursor];

	SDL_Color white = { 235, 235, 240, 255 };
	SDL_Color dim = { 140, 140, 150, 255 };
	SDL_Color hi = { 255, 255, 255, 255 };
	int x = 90;
	int box = EROS_SCREEN_W - 2 * x;
	draw_line_marquee(r, a->font, al->name, x, 60, box, white, 0,
	                  marquee_clock(a, 0, a->album_cursor));

	int row_h = 52;
	int top = 150;
	int visible = (EROS_SCREEN_H - top - 40) / row_h;
	int first = 0;
	if (a->track_cursor >= visible) first = a->track_cursor - visible + 1;

	for (int i = first; i < al->track_count && i < first + visible; i++) {
		int y = top + (i - first) * row_h;
		int pill_top = y - 4, pill_h = row_h - 8;
		bool sel = (i == a->track_cursor);
		bool playing = (a->play_album == a->album_cursor && a->play_track == i);
		if (sel) {
			SDL_SetRenderDrawColor(r, 255, 255, 255, 28);
			SDL_Rect pill = { x - 20, pill_top, EROS_SCREEN_W - 2 * (x - 20), pill_h };
			SDL_RenderFillRect(r, &pill);
		}
		char label[MUS_STR + 4];
		snprintf(label, sizeof label, "%s%s", playing ? "▸ " : "", al->tracks[i].name);
		/* cap-height center the text in the row so it doesn't ride high in the pill */
		int ty = EROS_textCenterY(a->font, pill_top, pill_h);
		/* only the highlighted row scrolls; the rest stay still and truncate */
		if (sel) draw_line_marquee(r, a->font, label, x, ty, box, hi, 0,
		                           marquee_clock(a, 2, a->track_cursor));
		else draw_line_trunc(r, a->font, label, x, ty, box, dim, 0);
	}
}

static void draw_progress(SDL_Renderer *r, int x, int y, int w, double pos, double dur)
{
	SDL_SetRenderDrawColor(r, 60, 62, 72, 255);
	SDL_Rect bg = { x, y, w, 6 };
	SDL_RenderFillRect(r, &bg);
	if (dur > 0.0) {
		int fw = (int)(w * (pos / dur));
		if (fw < 0) fw = 0;
		if (fw > w) fw = w;
		SDL_SetRenderDrawColor(r, 235, 235, 240, 255);
		SDL_Rect fg = { x, y, fw, 6 };
		SDL_RenderFillRect(r, &fg);
	}
}

static void fmt_time(double s, char *out, size_t n)
{
	if (s < 0) s = 0;
	int t = (int)s;
	snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

static void render_nowplaying(app *a)
{
	SDL_Renderer *r = plat_renderer();
	draw_bg(r);
	if (a->play_album < 0) return;
	mus_album *al = &a->lib.albums[a->play_album];
	mus_track *tr = &al->tracks[a->play_track];

	/* cover, centered upper area. NP_MARGIN is the top inset; PAUSED/PLAYING is
	 * anchored the same distance from the bottom edge for a symmetric frame. */
	const int NP_MARGIN = 40;
	int w, h;
	SDL_Texture *cov = cover_get(a, a->play_album, &w, &h);
	int side = 500;
	SDL_Rect dst = { (EROS_SCREEN_W - side) / 2, NP_MARGIN, side, side };
	if (cov) SDL_RenderCopy(r, cov, NULL, &dst);

	SDL_Color white = { 240, 240, 245, 255 };
	SDL_Color dim = { 150, 150, 160, 255 };
	/* just the song title, floated in the air below the cover. The artist and
	 * album are already shown on the way in - artist over the album cover, album
	 * atop the track list - so now-playing (the finest grain) needn't repeat them. */
	Uint32 mq = marquee_clock(a, 1, a->play_track);
	draw_line_marquee(r, a->font_title, tr->name, 50, 565, EROS_SCREEN_W - 100, white, 1, mq);

	/* "low-res" tag, bottom-right of the art, when the source image is small
	 * (embedded thumbnails etc. get upscaled to the 440px frame) */
	if (cov && w > 0 && w < LOWRES_MAX && h < LOWRES_MAX) {
		int lw = 0, lh = 0;
		TTF_SizeUTF8(a->font_meta, "low-res", &lw, &lh);
		SDL_Rect bg = { dst.x + dst.w - lw - 22, dst.y + dst.h - lh - 14, lw + 14, lh + 8 };
		SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(r, 0, 0, 0, 150);
		SDL_RenderFillRect(r, &bg);
		SDL_Color tag = { 215, 215, 220, 255 };
		draw_text(r, a->font_meta, "low-res", bg.x + 7, bg.y + 4, tag, 0, 0);
	}

	double pos = a->ps.pos, dur = a->ps.dur;
	int px = 160, pw = EROS_SCREEN_W - 2 * px;
	draw_progress(r, px, 640, pw, pos, dur);
	char a_t[16], b_t[16];
	fmt_time(pos, a_t, sizeof a_t);
	fmt_time(dur, b_t, sizeof b_t);
	draw_text(r, a->font_meta, a_t, px, 652, dim, 0, 0);
	draw_text(r, a->font_meta, b_t, EROS_SCREEN_W - px - 60, 652, dim, 0, 0);

	/* repeat/shuffle mode glyph, centered between the time row and the play state
	 * (off / "album ends" shows nothing, keeping the common case uncluttered) */
	float gcy = 676.0f, gs = 13.0f, gw = 3.0f;
	if (a->ps.mode == 1 || a->ps.mode == 2) {
		/* the repeat loop with an "all" / "1" label beside it, the pair centered */
		const char *lbl = a->ps.mode == 2 ? "1" : "all";
		int lw = 0, lh = 0;
		TTF_SizeUTF8(a->font_meta, lbl, &lw, &lh);
		float iconw = gs * 2.0f, gap = 10.0f, total = iconw + gap + lw;
		float x0 = EROS_SCREEN_W / 2.0f - total / 2.0f;
		icon_repeat(r, x0 + iconw / 2.0f, gcy, gs, gw, dim);
		/* the label has no descenders, so centering its full box on gcy would ride
		 * high; anchor its ink instead by dropping the baseline half an ascent. */
		int asc = TTF_FontAscent(a->font_meta);
		draw_text(r, a->font_meta, lbl, (int)(x0 + iconw + gap), (int)(gcy - asc / 2.0f), dim, 0, 0);
	} else if (a->ps.mode == 3) {
		icon_shuffle(r, EROS_SCREEN_W / 2.0f, gcy, gs, gw, dim);
	}

	/* play state; a stopped album (off mode, played through) reads as paused.
	 * Anchor the baseline NP_MARGIN from the bottom: the caps have no descenders,
	 * so their visual bottom lands exactly NP_MARGIN from the edge. */
	const char *base = (a->ps.paused || !a->ps.active) ? "∥  PAUSED" : "▶  PLAYING";
	draw_text(r, a->font_meta, base, EROS_SCREEN_W / 2,
	          EROS_SCREEN_H - NP_MARGIN - TTF_FontAscent(a->font_meta), dim, 1, 0);
}

/* ---- update ---- */

static void update_albums(app *a)
{
	in_state *in = &a->in;
	int n = a->lib.count;
	if (in_repeat(in, IN_LEFT)) a->album_cursor = (a->album_cursor - 1 + n) % n;
	if (in_repeat(in, IN_RIGHT)) a->album_cursor = (a->album_cursor + 1) % n;
	if (in->pressed[IN_ACCEPT]) {
		a->screen = SCREEN_TRACKS;
		a->track_cursor = 0;
	}
	/* B at the top level = leave AND stop the music (MENU leaves it playing).
	 * This is the way to fully stop music so games get their audio back. */
	if (in->pressed[IN_BACK]) { player_quit(); a->running = false; return; }
	/* START jumps straight to now-playing when something's playing */
	/* START or X jumps to now-playing (X is the mode toggle only on that screen) */
	if (in->pressed[IN_X] && a->play_album >= 0) { a->screen = SCREEN_NOWPLAYING; return; }
	cf_set_cursor(&a->cf, a->album_cursor, n);
}

static void update_tracks(app *a)
{
	in_state *in = &a->in;
	mus_album *al = &a->lib.albums[a->album_cursor];
	int n = al->track_count;
	if (in_repeat(in, IN_UP)) a->track_cursor = (a->track_cursor - 1 + n) % n;
	if (in_repeat(in, IN_DOWN)) a->track_cursor = (a->track_cursor + 1) % n;
	if (in->pressed[IN_BACK]) { a->screen = SCREEN_ALBUMS; return; }
	/* START or X jumps to now-playing (X is the mode toggle only on that screen) */
	if (in->pressed[IN_X] && a->play_album >= 0) { a->screen = SCREEN_NOWPLAYING; return; }
	if (in->pressed[IN_ACCEPT]) {
		/* picking the track that's already playing just opens now-playing -- don't
		 * restart it from the top (also the natural way back to now-playing) */
		if (!(a->play_album == a->album_cursor && a->play_track == a->track_cursor))
			play_index(a, a->album_cursor, a->track_cursor);
		a->screen = SCREEN_NOWPLAYING;
	}
}

static void update_nowplaying(app *a)
{
	in_state *in = &a->in;
	if (in->pressed[IN_BACK]) { a->screen = SCREEN_TRACKS; return; }
	if (in->pressed[IN_Y]) { player_stop(); a->play_album = -1; a->screen = SCREEN_TRACKS; return; }
	if (in->pressed[IN_ACCEPT]) {
		if (a->ps.paused) player_resume();
		else player_pause();
	}
	if (in->pressed[IN_X]) {   /* cycle off -> repeat-all -> repeat-one -> shuffle */
		int m = (a->ps.mode + 1) % 4;
		player_mode(m);
		a->ps.mode = m;   /* optimistic; the daemon's next status confirms it */
	}
	if (in->pressed[IN_LEFT]) {
		/* first press restarts the current track; only near the start does it
		 * skip to the previous one (standard music-player behavior) */
		if (a->ps.pos > 3.0) player_seek(-(a->ps.pos + 1.0));
		else play_step(a, -1);
	}
	if (in->pressed[IN_RIGHT]) play_step(a, +1);
	if (in_repeat(in, IN_L1)) player_seek(-5.0);
	if (in_repeat(in, IN_R1)) player_seek(+5.0);
}

/* ---- font resolution ---- */

static TTF_Font *open_font(int size)
{
	/* NOTE: iterate a fixed count and skip NULL/empty entries -- a plain
	 * `for (; candidates[i]; )` would stop at the first NULL, and the env
	 * override is NULL whenever EROS_MUSIC_FONT is unset (i.e. on device). */
	const char *candidates[] = { getenv("EROS_MUSIC_FONT"),
	                             "/mnt/SDCARD/eros/menu.ttf", P_FONT };
	for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
		if (!candidates[i] || !candidates[i][0]) continue;
		TTF_Font *f = TTF_OpenFont(candidates[i], size);
		if (f) return f;
	}
	return NULL;
}

int main(int argc, char *argv[])
{
	/* background audio daemon: owns the audio device, keeps playing after the UI
	 * closes. Spawned on demand by player_ensure_running(). */
	if (argc >= 2 && strcmp(argv[1], "--daemon") == 0)
		return player_daemon_main();

	player_set_self(argv[0]); /* so the daemon can be re-exec'd (native fallback) */
	(void)argc;
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	signal(SIGUSR1, on_usr1);
	script_init();

	app a = {0};
	for (int i = 0; i < 3; i++) a.mq_key[i] = -1;   /* force first marquee_clock to reset */
	paths_init();

	const char *root = getenv("MUSIC_PATH");
	char rootbuf[MUS_PATH];
	if (!root || !root[0]) {
		const char *sd = getenv("SDCARD_PATH");
		snprintf(rootbuf, sizeof rootbuf, "%s/Music", sd && sd[0] ? sd : "/mnt/SDCARD");
		root = rootbuf;
	}
	/* Cache the parsed library so re-opens skip the slow per-track tag read; the
	 * cache lives in userdata and is keyed on a stat signature of the tree. */
	char cachebuf[MUS_PATH];
	const char *ud = getenv("USERDATA_PATH");
	if (ud && ud[0])
		snprintf(cachebuf, sizeof cachebuf, "%s/muse_lib.cache", ud);
	else {
		const char *sd = getenv("SDCARD_PATH");
		snprintf(cachebuf, sizeof cachebuf, "%s/.userdata/tg5040/muse_lib.cache",
		         sd && sd[0] ? sd : "/mnt/SDCARD");
	}
	if (!library_scan_cached(root, cachebuf, &a.lib)) {
		fprintf(stderr, "no music found under %s\n", root);
		/* still bring up video so the user sees a message instead of a black exit */
	}

	if (!plat_video_init()) return 1;
	if (!plat_input_init()) return 1;
	IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
	if (TTF_Init() != 0) fprintf(stderr, "ttf: %s\n", TTF_GetError());
	a.font = open_font(34);
	a.font_small = open_font(24);
	a.font_title = open_font(46);
	a.font_album = open_font(38);
	a.font_meta = open_font(30);

	player_ensure_running(); /* audio lives in the daemon, not here */

	plat_leds_off();
	/* The client does NOT call plat_settings_init(): the daemon owns
	 * volume/brightness (InitSettings there, once, before audio) so re-opening the
	 * UI never re-inits the codec and pops the volume. The client nudges via the
	 * daemon (player_volume/brightness) and reads the levels back from status. */

	if (a.lib.count > 0) {
		a.cover = calloc(a.lib.count, sizeof *a.cover);
		a.cover_w = calloc(a.lib.count, sizeof *a.cover_w);
		a.cover_h = calloc(a.lib.count, sizeof *a.cover_h);
		a.placeholder = make_placeholder(plat_renderer(), &a.ph_w, &a.ph_h);
		cover_decode_start(&a);   /* worker to decode covers off the render thread */
	}
	a.play_album = -1;
	cf_reset(&a.cf, 0);
	if (a.lib.count > 0) cover_prime(&a); /* warm the first covers so early scrolls are smooth */

	/* Reconnect: if the daemon is already playing (backgrounded music), jump
	 * straight to its album/track on the now-playing screen. */
	if (player_get(&a.ps) && a.ps.active && a.ps.dir[0]) {
		for (int i = 0; i < a.lib.count; i++) {
			if (strcmp(a.lib.albums[i].dir, a.ps.dir) == 0) {
				a.play_album = i;
				a.album_cursor = i;
				a.play_track = a.ps.track;
				a.screen = SCREEN_NOWPLAYING;
				cf_reset(&a.cf, i);
				break;
			}
		}
	}
	cover_prefetch(&a);   /* warm covers around the real starting cursor */
	a.running = true;

	Uint32 grace_until = SDL_GetTicks() + 400;

	while (a.running) {
		plat_input_poll(&a.in);
		if (SDL_GetTicks() < grace_until)
			memset(a.in.pressed, 0, sizeof a.in.pressed);
		script_step(&a.in);
		if (a.in.quit_requested || want_quit) break;
		if (a.in.pressed[IN_MENU]) break;   /* MENU quits back to the launcher */
		if (a.in.pressed[IN_POWER]) { plat_request_poweroff(); break; }

		/* Volume/brightness go through the daemon (which owns the settings
		 * device); show the OSD immediately at the expected level, which the
		 * daemon's next status report confirms. */
		if (in_repeat(&a.in, IN_VOLUP))    { player_volume(+1);     plat_osd_show(2, a.ps.vol + 1, 20); }
		if (in_repeat(&a.in, IN_VOLDN))    { player_volume(-1);     plat_osd_show(2, a.ps.vol - 1, 20); }
		if (in_repeat(&a.in, IN_BRIGHTUP)) { player_brightness(+1); plat_osd_show(1, a.ps.bright + 1, 10); }
		if (in_repeat(&a.in, IN_BRIGHTDN)) { player_brightness(-1); plat_osd_show(1, a.ps.bright - 1, 10); }

		/* refresh the background player's state (position, paused, and the
		 * current track after the daemon auto-advances) for display */
		int prev_mode = a.ps.mode;
		bool got = player_get(&a.ps);
		a.ps.mode = prev_mode;   /* mode is client-authoritative: the daemon only
		                          * echoes it back a beat later, and reading that
		                          * stale echo would flash the glyph to the old mode */
		if (got && a.ps.active && a.play_album >= 0) a.play_track = a.ps.track;

		/* Pairing is launcher-owned (see src/btpair.c): when the switch is on BT
		 * but no sink is connected, Muse just plays to the (silent) BT output --
		 * back out to the launcher to pair or reconnect a headset. */

		if (a.lib.count == 0) {
			SDL_Renderer *r = plat_renderer();
			draw_bg(r);
			SDL_Color dim = { 160, 160, 170, 255 };
			draw_text(r, a.font, "No music found", EROS_SCREEN_W / 2, EROS_SCREEN_H / 2 - 20,
			          dim, 1, 0);
			draw_text(r, a.font_small, "put tracks under /mnt/SDCARD/Music",
			          EROS_SCREEN_W / 2, EROS_SCREEN_H / 2 + 30, dim, 1, 0);
			SDL_RenderPresent(r);
			SDL_Delay(16);
			continue;
		}

		switch (a.screen) {
		case SCREEN_ALBUMS: update_albums(&a); break;
		case SCREEN_TRACKS: update_tracks(&a); break;
		case SCREEN_NOWPLAYING: update_nowplaying(&a); break;
		}
		if (!a.running) break;

		switch (a.screen) {
		case SCREEN_ALBUMS: render_albums(&a); break;
		case SCREEN_TRACKS: render_tracks(&a); break;
		case SCREEN_NOWPLAYING: render_nowplaying(&a); break;
		}
		if (want_shot) { want_shot = 0; save_shot(plat_renderer()); }
		plat_draw_osd(plat_renderer());   /* volume/brightness line, on top */
		SDL_RenderPresent(plat_renderer());
		SDL_Delay(8);
	}

	cover_decode_stop(&a);   /* join the worker before freeing what it reads */
	if (a.placeholder) SDL_DestroyTexture(a.placeholder);
	if (a.cover) {
		for (int i = 0; i < a.lib.count; i++)
			if (a.cover[i]) SDL_DestroyTexture(a.cover[i]);
		free(a.cover); free(a.cover_w); free(a.cover_h);
	}
	library_free(&a.lib);
	if (a.font) TTF_CloseFont(a.font);
	if (a.font_small) TTF_CloseFont(a.font_small);
	if (a.font_title) TTF_CloseFont(a.font_title);
	if (a.font_album) TTF_CloseFont(a.font_album);
	if (a.font_meta) TTF_CloseFont(a.font_meta);
	TTF_Quit();
	IMG_Quit();
	plat_input_quit();
	plat_video_quit();
	SDL_Quit();
	return 0;
}
