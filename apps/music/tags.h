#ifndef EROS_MUSIC_TAGS_H
#define EROS_MUSIC_TAGS_H

#include <stdbool.h>

#define TAG_STR 256

/* Metadata pulled from a track's tags. Every string field is UTF-8 and is left
 * "" when the tag is absent; track_no is 0 when unknown. */
typedef struct {
	char title[TAG_STR];
	char artist[TAG_STR];
	char album[TAG_STR];
	int  track_no;
} mus_tags;

/* Read title/artist/album/track from a file's tags: ID3v2 for .mp3, Vorbis
 * comments for .flac/.ogg/.oga. Fields not found stay "". Returns true if the
 * format was recognized and at least one field was filled. */
bool tags_read(const char *path, mus_tags *out);

/* Extract embedded cover art (ID3v2 APIC / FLAC PICTURE / Vorbis
 * METADATA_BLOCK_PICTURE) into a freshly malloc'd buffer. Returns the image
 * bytes (caller frees) and writes the length to *len, or NULL if none. The
 * bytes are a raw JPEG/PNG image, loadable straight from memory. */
unsigned char *tags_read_art(const char *path, int *len);

#endif /* EROS_MUSIC_TAGS_H */
