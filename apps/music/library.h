#ifndef EROS_MUSIC_LIBRARY_H
#define EROS_MUSIC_LIBRARY_H

#include <stdbool.h>

#define MUS_STR  256
#define MUS_PATH 1024

typedef struct {
	char name[MUS_STR];   /* display name: tag title, else filename (no ext/track no) */
	char path[MUS_PATH];  /* full path to the audio file */
	int  track_no;        /* tag track number, or 0 (used to order the list) */
} mus_track;

typedef struct {
	char name[MUS_STR];    /* album display name: tag album, else folder name */
	char artist[MUS_STR];  /* album artist from tags, or "" */
	char dir[MUS_PATH];    /* the folder holding the tracks */
	char cover[MUS_PATH];  /* full path to a cover image, or "" if none */
	unsigned char *art;    /* embedded cover art bytes (when no cover file), or NULL */
	int art_len;           /* length of `art` */
	bool is_singles;       /* loose files from Music/ root -> the "Singles" bucket */
	bool branded;          /* draw the power+wordmark cover (Singles, or a coverless collection) */
	mus_track *tracks;
	int track_count;
} mus_album;

typedef struct {
	mus_album *albums;
	int count;
} mus_library;

/* Recursively scan `root`; every folder that directly holds audio files becomes
 * an album. Returns false only if nothing playable was found. */
bool library_scan(const char *root, mus_library *lib);

/* Like library_scan, but backed by an on-disk cache at `cache_path`: reloads the
 * cache when the folder tree is unchanged (skipping the slow per-track tag read),
 * else does a full scan and refreshes the cache. Used for the whole-library open. */
bool library_scan_cached(const char *root, const char *cache_path, mus_library *lib);

void library_free(mus_library *lib);

#endif /* EROS_MUSIC_LIBRARY_H */
