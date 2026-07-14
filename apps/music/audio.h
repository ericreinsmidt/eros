#ifndef EROS_MUSIC_AUDIO_H
#define EROS_MUSIC_AUDIO_H

#include <stdbool.h>

/* Audio backend for the EROS music player. Decodes WAV / MP3 / FLAC / OGG via
 * vendored single-header decoders (dr_wav / dr_mp3 / dr_flac / stb_vorbis). On
 * the device, output is raw ALSA so it can target a specific PCM -- "default"
 * (speaker) or a "bluealsa:DEV=<mac>,PROFILE=a2dp" Bluetooth sink -- and switch
 * between them live via audio_set_device(). (The native dev build uses SDL.) */

bool   audio_init(void);
void   audio_quit(void);

/* Select the output PCM: "default" for the speaker, or a
 * "bluealsa:DEV=<mac>,PROFILE=a2dp" string for a connected BT sink. Switches
 * live if something is playing, otherwise applies to the next track. If the
 * chosen device can't be opened, playback falls back to the speaker. */
void   audio_set_device(const char *dev);

/* Start playing a file. Returns false if it could not be opened/decoded. */
bool   audio_play(const char *path);
void   audio_toggle_pause(void);
void   audio_pause(void);
void   audio_resume(void);
void   audio_stop(void);

bool   audio_is_paused(void);
bool   audio_is_playing(void);   /* device open, not paused, not finished */
bool   audio_finished(void);     /* current track reached its end */

double audio_position(void);     /* seconds into the track */
double audio_duration(void);     /* track length in seconds, 0 if unknown */
void   audio_seek(double delta_seconds);   /* relative, clamped to the track */

#endif /* EROS_MUSIC_AUDIO_H */
