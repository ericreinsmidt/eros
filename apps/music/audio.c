#include "audio.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Single-header decoders. dr_* implementations are compiled here; stb_vorbis's
 * implementation is its own translation unit (third_party/stb_vorbis.c), so we
 * pull in only its header. */
#define DR_WAV_IMPLEMENTATION
#include "third_party/dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#include "third_party/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "third_party/dr_flac.h"
#define STB_VORBIS_HEADER_ONLY
#include "third_party/stb_vorbis.c"

/* ---- decoder abstraction: WAV / MP3 / FLAC / OGG behind a uniform S16 read.
 * Picked by file extension; the playback thread then only ever calls
 * dec_read(). ---- */
typedef enum { DEC_NONE = 0, DEC_WAV, DEC_MP3, DEC_FLAC, DEC_OGG } dec_kind;

static dec_kind    s_kind = DEC_NONE;
static drwav       s_wav;
static drmp3       s_mp3;
static drflac     *s_flac = NULL;
static stb_vorbis *s_ogg = NULL;
static int         s_ogg_ch = 0;

static dec_kind kind_for(const char *path)
{
	const char *dot = strrchr(path, '.');
	if (!dot) return DEC_NONE;
	if (strcasecmp(dot, ".wav") == 0) return DEC_WAV;
	if (strcasecmp(dot, ".mp3") == 0) return DEC_MP3;
	if (strcasecmp(dot, ".flac") == 0) return DEC_FLAC;
	if (strcasecmp(dot, ".ogg") == 0 || strcasecmp(dot, ".oga") == 0) return DEC_OGG;
	return DEC_NONE;
}

bool audio_handles(const char *path) { return kind_for(path) != DEC_NONE; }

static bool dec_open(const char *path, unsigned *ch, unsigned *rate, uint64_t *total)
{
	s_kind = kind_for(path);
	switch (s_kind) {
	case DEC_WAV:
		if (!drwav_init_file(&s_wav, path, NULL)) break;
		*ch = s_wav.channels; *rate = s_wav.sampleRate; *total = s_wav.totalPCMFrameCount;
		return true;
	case DEC_MP3:
		if (!drmp3_init_file(&s_mp3, path, NULL)) break;
		*ch = s_mp3.channels; *rate = s_mp3.sampleRate;
		*total = drmp3_get_pcm_frame_count(&s_mp3);   /* scans the file... */
		drmp3_seek_to_pcm_frame(&s_mp3, 0);           /* ...so rewind */
		return true;
	case DEC_FLAC:
		s_flac = drflac_open_file(path, NULL);
		if (!s_flac) break;
		*ch = s_flac->channels; *rate = s_flac->sampleRate; *total = s_flac->totalPCMFrameCount;
		return true;
	case DEC_OGG: {
		int err = 0;
		s_ogg = stb_vorbis_open_filename(path, &err, NULL);
		if (!s_ogg) break;
		stb_vorbis_info info = stb_vorbis_get_info(s_ogg);
		s_ogg_ch = info.channels;
		*ch = (unsigned)info.channels; *rate = info.sample_rate;
		*total = stb_vorbis_stream_length_in_samples(s_ogg);
		return true;
	}
	default: break;
	}
	s_kind = DEC_NONE;
	return false;
}

/* Read up to `frames` interleaved S16 frames; returns frames decoded (0 = EOF). */
static uint64_t dec_read(int16_t *buf, uint64_t frames)
{
	switch (s_kind) {
	case DEC_WAV:  return drwav_read_pcm_frames_s16(&s_wav, frames, buf);
	case DEC_MP3:  return drmp3_read_pcm_frames_s16(&s_mp3, frames, buf);
	case DEC_FLAC: return drflac_read_pcm_frames_s16(s_flac, frames, buf);
	case DEC_OGG: {
		int got = stb_vorbis_get_samples_short_interleaved(
		              s_ogg, s_ogg_ch, buf, (int)(frames * (uint64_t)s_ogg_ch));
		return got < 0 ? 0 : (uint64_t)got;
	}
	default: return 0;
	}
}

static void dec_seek_frame(uint64_t f)
{
	switch (s_kind) {
	case DEC_WAV:  drwav_seek_to_pcm_frame(&s_wav, f); break;
	case DEC_MP3:  drmp3_seek_to_pcm_frame(&s_mp3, f); break;
	case DEC_FLAC: drflac_seek_to_pcm_frame(s_flac, f); break;
	case DEC_OGG:  if (s_ogg) stb_vorbis_seek_frame(s_ogg, (unsigned)f); break;
	default: break;
	}
}

static void dec_close(void)
{
	switch (s_kind) {
	case DEC_WAV:  drwav_uninit(&s_wav); break;
	case DEC_MP3:  drmp3_uninit(&s_mp3); break;
	case DEC_FLAC: if (s_flac) { drflac_close(s_flac); s_flac = NULL; } break;
	case DEC_OGG:  if (s_ogg) { stb_vorbis_close(s_ogg); s_ogg = NULL; } break;
	default: break;
	}
	s_kind = DEC_NONE;
}

/* ============================================================================
 * Output. On the device this is raw ALSA so we can target a specific PCM --
 * "default" (speaker) or "bluealsa:DEV=<mac>,PROFILE=a2dp" (a connected BT sink)
 * -- and switch between them live. SDL2 can't open the bluealsa plugin (it only
 * opens enumerated cards), hence raw ALSA. The native (desktop) dev build keeps
 * the simpler SDL path since it never needs Bluetooth. Shared state:
 * ==========================================================================*/
static unsigned          s_channels = 0;
static unsigned          s_rate = 0;
static uint64_t          s_total = 0;      /* total frames, 0 = unknown */
static volatile uint64_t s_cursor = 0;     /* frames decoded so far */
static volatile bool     s_paused = false;
static volatile bool     s_finished = false;
static volatile int      s_seek = 0;
static volatile uint64_t s_seek_target = 0;

#ifdef __linux__
/* ---- raw ALSA output (adapted from EROS's DiscoBoy audio backend) ---- */
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

static snd_pcm_t   *s_pcm = NULL;
static bool         s_open = false;
static pthread_t    s_thread;
static bool         s_have_thread = false;
static volatile bool s_stop = false;
static char          s_dev[256]  = "default";  /* device the pcm is currently on */
static char          s_want[256] = "default";  /* device we want to be on */
static volatile int  s_reopen = 0;

/* Open `dev` and, on success, swap the live pcm to it (keeping the decoder
 * position). On failure the current pcm/device is left untouched. */
static bool pcm_open(const char *dev)
{
	snd_pcm_t *p = NULL;
	if (snd_pcm_open(&p, dev, SND_PCM_STREAM_PLAYBACK, 0) < 0) return false;
	if (snd_pcm_set_params(p, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED,
	                       s_channels, s_rate, 1, 500000) < 0) { /* 500ms buffer:
	     enough decoded audio queued to ride through the CPU/IO spike when a game
	     launches (core dlopen + ROM load) without the pcm underrunning. */
		snd_pcm_close(p);
		return false;
	}
	if (s_pcm) { snd_pcm_drop(s_pcm); snd_pcm_close(s_pcm); }
	s_pcm = p;
	snprintf(s_dev, sizeof s_dev, "%s", dev);
	return true;
}

static void do_reopen(void)   /* runs in the playback thread -- no pcm race */
{
	if (strcmp(s_want, s_dev) == 0) return;
	pcm_open(s_want);   /* on failure keeps the current device (speaker fallback) */
}

static void do_seek(void)
{
	dec_seek_frame(s_seek_target);
	s_cursor = s_seek_target;
	if (s_pcm) { snd_pcm_drop(s_pcm); snd_pcm_prepare(s_pcm); }
}

/* Ask the scheduler to run the playback thread at a (low) real-time priority so
 * it keeps refilling the pcm even when a game launch pegs the cores. It only ever
 * runs to decode a chunk and hand it to ALSA, then blocks in snd_pcm_writei, so
 * it can't starve the system. Best-effort: needs privilege (musicd runs as root
 * on the device); a plain failure just leaves it at normal priority. */
static void play_thread_go_realtime(void)
{
	struct sched_param sp;
	memset(&sp, 0, sizeof sp);
	sp.sched_priority = 10;   /* low RT band -- above normal work, below the kernel's */
	pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}

static void *play_thread(void *arg)
{
	(void)arg;
	play_thread_go_realtime();
	int16_t buf[8192];
	uint64_t cap = sizeof(buf) / sizeof(buf[0]) / (s_channels ? s_channels : 1);
	while (!s_stop) {
		if (s_paused) {
			snd_pcm_drop(s_pcm);
			while (s_paused && !s_stop) usleep(20000);
			if (s_stop) break;
			snd_pcm_prepare(s_pcm);
			continue;
		}
		if (s_reopen) { do_reopen(); s_reopen = 0; }
		if (s_seek)   { do_seek();   s_seek = 0; }
		uint64_t got = dec_read(buf, cap);
		if (got == 0) { s_finished = true; break; }
		uint64_t off = 0;
		while (off < got && !s_stop) {
			snd_pcm_sframes_t w =
			    snd_pcm_writei(s_pcm, buf + off * s_channels, (snd_pcm_uframes_t)(got - off));
			if (w < 0) {   /* xrun/underrun -> try to recover and keep going */
				w = snd_pcm_recover(s_pcm, (int)w, 1);
				if (w < 0) { s_finished = true; goto done; }
				continue;
			}
			off += (uint64_t)w;
		}
		s_cursor += got;
	}
done:
	/* Natural end: let the queued buffer play out. Stopped (skip/quit): drop it,
	 * so the bigger buffer doesn't leave a half-second tail of the old track. */
	if (s_pcm) { if (s_stop) snd_pcm_drop(s_pcm); else snd_pcm_drain(s_pcm); }
	return NULL;
}

bool audio_init(void) { return true; }   /* ALSA opens lazily on play */
void audio_quit(void) { audio_stop(); }

void audio_stop(void)
{
	if (s_have_thread) { s_stop = true; pthread_join(s_thread, NULL); s_have_thread = false; }
	if (s_pcm)  { snd_pcm_close(s_pcm); s_pcm = NULL; }
	if (s_open) { dec_close(); s_open = false; }
	s_channels = s_rate = 0;
	s_total = s_cursor = 0;
	s_paused = false;
	s_finished = false;
	s_seek = s_reopen = 0;
}

/* Select the output device: "default" (speaker) or a "bluealsa:DEV=..." string.
 * Switches live if playing, else applies to the next track. */
void audio_set_device(const char *dev)
{
	if (!dev || !dev[0]) return;
	snprintf(s_want, sizeof s_want, "%s", dev);
	if (s_have_thread) s_reopen = 1;                       /* thread swaps it */
	else snprintf(s_dev, sizeof s_dev, "%s", dev);         /* applies on next open */
}

bool audio_play(const char *path)
{
	/* Stop the playback thread but KEEP the pcm open, so the next track can reuse
	 * it when the sink + format are unchanged. Closing/reopening the device every
	 * track tore down and re-established the bluealsa sink each time -- audibly
	 * bouncing BT<->speaker and dropping between tracks. */
	if (s_have_thread) { s_stop = true; pthread_join(s_thread, NULL); s_have_thread = false; }
	if (s_open) { dec_close(); s_open = false; }

	unsigned ch = 0, rate = 0;
	uint64_t total = 0;
	if (!dec_open(path, &ch, &rate, &total)) return false;
	if (ch == 0 || rate == 0) { dec_close(); return false; }

	const char *dev = s_want[0] ? s_want : "default";
	bool reuse = s_pcm && strcmp(s_dev, dev) == 0 && ch == s_channels && rate == s_rate;

	s_channels = ch; s_rate = rate; s_total = total; s_cursor = 0;
	s_open = true;
	s_paused = s_stop = s_finished = false;
	s_seek = s_reopen = 0;

	if (reuse) {
		snd_pcm_drop(s_pcm);      /* clear the drained/old state ... */
		snd_pcm_prepare(s_pcm);   /* ... and ready it for the new track's frames */
	} else if (!pcm_open(dev)) {
		/* wanted device (e.g. a BT sink) wouldn't open -> fall back to speaker */
		if (strcmp(dev, "default") == 0 || !pcm_open("default")) {
			dec_close(); s_open = false; return false;
		}
	}
	if (pthread_create(&s_thread, NULL, play_thread, NULL) != 0) {
		if (s_pcm) { snd_pcm_close(s_pcm); s_pcm = NULL; }
		dec_close(); s_open = false; return false;
	}
	s_have_thread = true;
	return true;
}

void audio_pause(void)  { s_paused = true; }
void audio_resume(void) { s_paused = false; }
void audio_toggle_pause(void) { if (!s_finished) s_paused = !s_paused; }

bool audio_is_paused(void)  { return s_have_thread && s_paused; }
bool audio_is_playing(void) { return s_have_thread && !s_paused && !s_finished; }
bool audio_finished(void)   { return s_finished; }

void audio_seek(double delta_seconds)
{
	if (!s_have_thread || !s_rate) return;
	long long tgt = (long long)s_cursor + (long long)(delta_seconds * (double)s_rate);
	if (tgt < 0) tgt = 0;
	if (s_total && (uint64_t)tgt > s_total) tgt = (long long)s_total;
	s_seek_target = (uint64_t)tgt;
	s_finished = false;
	s_seek = 1;
}

#else /* !__linux__ : native desktop dev build -- SDL output, no Bluetooth ---- */
#include <SDL.h>

static SDL_AudioDeviceID s_dev = 0;

static void audio_cb(void *ud, Uint8 *stream, int len)
{
	(void)ud;
	int16_t *out = (int16_t *)stream;
	uint64_t want = (uint64_t)len / (s_channels ? s_channels * sizeof(int16_t) : sizeof(int16_t));
	if (s_paused || s_finished) { memset(stream, 0, len); return; }
	if (s_seek) { dec_seek_frame(s_seek_target); s_cursor = s_seek_target; s_seek = 0; }
	uint64_t got = dec_read(out, want);
	if (got < want) {
		memset(out + got * s_channels, 0, (size_t)(want - got) * s_channels * sizeof(int16_t));
		if (got == 0) s_finished = true;
	}
	s_cursor += got;
}

bool audio_init(void) { return SDL_InitSubSystem(SDL_INIT_AUDIO) == 0; }
void audio_quit(void) { audio_stop(); SDL_QuitSubSystem(SDL_INIT_AUDIO); }
void audio_set_device(const char *dev) { (void)dev; }   /* no BT on the dev build */

void audio_stop(void)
{
	if (s_dev) { SDL_CloseAudioDevice(s_dev); s_dev = 0; }
	if (s_kind != DEC_NONE) dec_close();
	s_channels = s_rate = 0;
	s_total = s_cursor = 0;
	s_paused = false; s_finished = false; s_seek = 0;
}

bool audio_play(const char *path)
{
	audio_stop();
	unsigned ch = 0, rate = 0; uint64_t total = 0;
	if (!dec_open(path, &ch, &rate, &total)) return false;
	if (ch == 0 || rate == 0) { dec_close(); return false; }

	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = (int)rate; want.format = AUDIO_S16SYS; want.channels = (Uint8)ch;
	want.samples = 4096; want.callback = audio_cb;
	s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (!s_dev) { dec_close(); return false; }

	s_channels = ch; s_rate = rate; s_total = total; s_cursor = 0;
	s_paused = false; s_finished = false; s_seek = 0;
	SDL_PauseAudioDevice(s_dev, 0);
	return true;
}

void audio_pause(void)  { if (s_dev) s_paused = true; }
void audio_resume(void) { if (s_dev) s_paused = false; }
void audio_toggle_pause(void) { if (s_dev && !s_finished) s_paused = !s_paused; }
bool audio_is_paused(void)  { return s_dev && s_paused; }
bool audio_is_playing(void) { return s_dev && !s_paused && !s_finished; }
bool audio_finished(void)   { return s_finished; }

void audio_seek(double delta_seconds)
{
	if (!s_dev || !s_rate) return;
	long long tgt = (long long)s_cursor + (long long)(delta_seconds * (double)s_rate);
	if (tgt < 0) tgt = 0;
	if (s_total && (uint64_t)tgt > s_total) tgt = (long long)s_total;
	SDL_LockAudioDevice(s_dev);
	s_seek_target = (uint64_t)tgt; s_seek = 1; s_finished = false;
	SDL_UnlockAudioDevice(s_dev);
}
#endif

double audio_duration(void) { return (s_rate && s_total) ? (double)s_total / s_rate : 0.0; }
double audio_position(void) { return s_rate ? (double)s_cursor / s_rate : 0.0; }
