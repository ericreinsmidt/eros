#include "library.h"
#include "tags.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static bool is_audio(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot) return false;
	return strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".flac") == 0 ||
	       strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".ogg") == 0 ||
	       strcasecmp(dot, ".oga") == 0;
}

/* Rank a cover-image candidate; higher wins, -1 = not a cover. A folder often
 * carries several: an intentional cover.jpg next to junk Windows-Media leftovers
 * (Folder.jpg, AlbumArtSmall.jpg) that are tiny and blurry. Prefer cover.* over
 * folder.*, png over jpg; file size breaks ties (bigger ~ higher res). This is
 * why we can't just take the first readdir match -- the junk often sorts first. */
static int cover_rank(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot) return -1;
	int ext;
	if (strcasecmp(dot, ".png") == 0) ext = 1;
	else if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) ext = 0;
	else return -1;
	if (strncasecmp(name, "cover.", 6) == 0) return 10 + ext;
	if (strncasecmp(name, "folder.", 7) == 0) return 5 + ext;
	return -1;   /* AlbumArtSmall.jpg etc. are not covers */
}

/* one file gathered during a folder scan, before ordering is decided */
typedef struct {
	char path[MUS_PATH];
	char fname[MUS_STR];   /* filename without extension: fallback sort key + name */
	char title[MUS_STR];   /* tag title, or "" */
	int  track_no;         /* tag track number, or 0 */
} scan_track;

/* order by tag track number when the whole folder is numbered, else by filename */
static int scan_cmp_num(const void *pa, const void *pb)
{
	const scan_track *a = pa, *b = pb;
	if (a->track_no != b->track_no) return a->track_no - b->track_no;
	return strcasecmp(a->fname, b->fname);
}
static int scan_cmp_name(const void *pa, const void *pb)
{
	return strcasecmp(((const scan_track *)pa)->fname, ((const scan_track *)pb)->fname);
}

/* sort key: the artist (a leading "The " ignored), falling back to the album
 * name when a folder had no artist tag */
static const char *artist_key(const mus_album *a)
{
	const char *s = a->artist[0] ? a->artist : a->name;
	if (strncasecmp(s, "the ", 4) == 0) s += 4;
	return s;
}

/* albums sort by artist, then by album name within an artist -- so a band's
 * records sit together on the shelf */
static int album_cmp(const void *pa, const void *pb)
{
	const mus_album *a = pa, *b = pb;
	int c = strcasecmp(artist_key(a), artist_key(b));
	return c ? c : strcasecmp(a->name, b->name);
}

/* Grow-able album list carried through the recursion. */
typedef struct {
	mus_album *items;
	int count, cap;
} album_vec;

static mus_album *album_vec_push(album_vec *v)
{
	if (v->count == v->cap) {
		v->cap = v->cap ? v->cap * 2 : 16;
		v->items = realloc(v->items, v->cap * sizeof *v->items);
	}
	mus_album *a = &v->items[v->count++];
	memset(a, 0, sizeof *a);
	return a;
}

/* Build one album from the audio files directly inside `dir`. */
/* Strip a leading track-number prefix from a display name, in place:
 * "01 Rhubarb" -> "Rhubarb", "1-10 Stylo" -> "Stylo", "03 - Foo" -> "Foo".
 * Requires digits followed by a separator (space/./-/_), so "20th Century Boy"
 * and a bare "01" are left alone. */
static void strip_track_number(char *name)
{
	char *p = name;
	while (*p == ' ') p++;
	char *q = p;
	bool had_digit = false;
	while (*q) {
		if (*q >= '0' && *q <= '9') { had_digit = true; q++; }
		else if (*q == '-' && q > p && q[1] >= '0' && q[1] <= '9') q++; /* 1-10 */
		else break;
	}
	if (!had_digit) return;
	char *sep = q;
	while (*sep == ' ' || *sep == '.' || *sep == '-' || *sep == '_') sep++;
	if (sep == q || *sep == '\0') return; /* no separator, or nothing left */
	memmove(name, sep, strlen(sep) + 1);
}

static void scan_dir_tracks(const char *dir, const char *display, album_vec *out)
{
	DIR *d = opendir(dir);
	if (!d) return;

	scan_track *st = NULL;
	int n = 0, cap = 0;
	char cover[MUS_PATH] = "";
	int cover_rk = -1;
	long cover_sz = -1;
	char album[MUS_STR] = "", artist[MUS_STR] = "";
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		int rk = cover_rank(e->d_name);
		if (rk >= 0) {   /* keep the best-ranked / largest cover, not the first */
			char cp[MUS_PATH];
			snprintf(cp, sizeof cp, "%s/%s", dir, e->d_name);
			struct stat cs;
			long sz = (stat(cp, &cs) == 0) ? (long)cs.st_size : 0;
			if (rk > cover_rk || (rk == cover_rk && sz > cover_sz)) {
				cover_rk = rk; cover_sz = sz;
				snprintf(cover, sizeof cover, "%s", cp);
			}
			continue;
		}
		if (!is_audio(e->d_name)) continue;
		char full[MUS_PATH];
		snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
		struct stat sb;
		if (stat(full, &sb) != 0 || !S_ISREG(sb.st_mode)) continue;
		if (n == cap) { cap = cap ? cap * 2 : 32; st = realloc(st, cap * sizeof *st); }

		mus_tags tg;
		bool have = tags_read(full, &tg);
		snprintf(st[n].path, sizeof st[n].path, "%s", full);
		snprintf(st[n].fname, sizeof st[n].fname, "%s", e->d_name);
		char *dot = strrchr(st[n].fname, '.');
		if (dot && dot != st[n].fname) *dot = '\0';
		snprintf(st[n].title, sizeof st[n].title, "%s", have ? tg.title : "");
		st[n].track_no = have ? tg.track_no : 0;
		/* the folder's album/artist come from the first track that carries them */
		if (have && !album[0] && tg.album[0]) snprintf(album, sizeof album, "%s", tg.album);
		if (have && !artist[0] && tg.artist[0]) snprintf(artist, sizeof artist, "%s", tg.artist);
		n++;
	}
	closedir(d);

	if (n == 0) { free(st); return; }

	/* If every track is numbered, trust the tag order; otherwise fall back to
	 * sorting by filename (which the leading track-number prefix usually sorts). */
	bool all_numbered = true;
	for (int i = 0; i < n; i++) if (st[i].track_no <= 0) { all_numbered = false; break; }
	qsort(st, n, sizeof *st, all_numbered ? scan_cmp_num : scan_cmp_name);

	mus_track *tracks = malloc(n * sizeof *tracks);
	for (int i = 0; i < n; i++) {
		snprintf(tracks[i].path, sizeof tracks[i].path, "%s", st[i].path);
		tracks[i].track_no = st[i].track_no;
		if (st[i].title[0]) {
			snprintf(tracks[i].name, sizeof tracks[i].name, "%s", st[i].title);
		} else {
			snprintf(tracks[i].name, sizeof tracks[i].name, "%s", st[i].fname);
			strip_track_number(tracks[i].name);   /* only filename-derived names */
		}
	}

	mus_album *a = album_vec_push(out);
	snprintf(a->name, sizeof a->name, "%s", album[0] ? album : display);
	snprintf(a->artist, sizeof a->artist, "%s", artist);
	snprintf(a->dir, sizeof a->dir, "%s", dir);
	snprintf(a->cover, sizeof a->cover, "%s", cover);
	/* no cover file in the folder -> fall back to art embedded in the first track */
	if (!cover[0]) a->art = tags_read_art(tracks[0].path, &a->art_len);
	a->tracks = tracks;
	a->track_count = n;
	free(st);
}

/* order tracks by full path == filename order (same folder): what a curated
 * playlist wants, rather than each track's original album track-number */
static int track_path_cmp(const void *pa, const void *pb)
{
	return strcasecmp(((const mus_track *)pa)->path, ((const mus_track *)pb)->path);
}

/* Music/Playlists/<Name>/ -> a collection: the folder name is the label, tracks
 * play in filename order, and the cover is the folder's cover.png if present else
 * a branded power+name cover. (Tracks still show their own tag titles.) */
static void scan_playlists(const char *dir, album_vec *out)
{
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char full[MUS_PATH * 2];
		snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
		struct stat st;
		if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
		int before = out->count;
		scan_dir_tracks(full, e->d_name, out);
		if (out->count == before) continue;   /* the collection folder had no audio */
		mus_album *a = &out->items[before];
		snprintf(a->name, sizeof a->name, "%s", e->d_name);   /* folder name is the label */
		a->artist[0] = '\0';
		qsort(a->tracks, a->track_count, sizeof *a->tracks, track_path_cmp);
		if (!a->cover[0]) {   /* no image in the folder -> branded power+name cover */
			free(a->art); a->art = NULL; a->art_len = 0;
			a->branded = true;
		}
	}
	closedir(d);
}

/* Depth-first walk: any folder that directly holds audio becomes an album, and
 * we still descend into subfolders (so Artist/Album/... trees flatten into one
 * album per leaf folder). */
static void walk(const char *dir, const char *display, album_vec *out, int depth)
{
	if (depth > 8) return;
	scan_dir_tracks(dir, display, out);

	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char full[MUS_PATH];
		snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
		struct stat st;
		if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
		walk(full, e->d_name, out, depth + 1);
	}
	closedir(d);
}

bool library_scan(const char *root, mus_library *lib)
{
	memset(lib, 0, sizeof *lib);
	album_vec v = {0};

	/* Audio files sitting loose in Music/ root become one branded "Singles"
	 * album -- name/artist forced, its cover drawn by the UI (not read from
	 * disk). Real albums each live in their own subfolder. */
	int before = v.count;
	scan_dir_tracks(root, "Singles", &v);
	if (v.count > before) {
		mus_album *s = &v.items[before];
		snprintf(s->name, sizeof s->name, "Singles");
		s->artist[0] = '\0';
		s->cover[0] = '\0';
		free(s->art); s->art = NULL; s->art_len = 0;
		s->is_singles = true;
		s->branded = true;
		qsort(s->tracks, s->track_count, sizeof *s->tracks, track_path_cmp);   /* filename order */
	}

	/* every subfolder is scanned as usual (a folder that holds audio = an album),
	 * except Music/Playlists/, whose children are collections */
	DIR *d = opendir(root);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.') continue;
			char full[MUS_PATH];
			snprintf(full, sizeof full, "%s/%s", root, e->d_name);
			struct stat st;
			if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
			if (strcasecmp(e->d_name, "Playlists") == 0) scan_playlists(full, &v);
			else walk(full, e->d_name, &v, 1);
		}
		closedir(d);
	}

	if (v.count == 0) { free(v.items); return false; }
	qsort(v.items, v.count, sizeof *v.items, album_cmp);
	lib->albums = v.items;
	lib->count = v.count;
	return true;
}

/* ---- library cache ----
 * The full scan reads tags from every track (cold = slow, the first-open lag).
 * Cache the parsed library to disk keyed on a cheap stat-only signature of the
 * folder tree; on the next open, if nothing changed, reload the cache instead of
 * re-reading every tag. Any change -- album added/removed/renamed, or a track
 * added/removed (which bumps its folder's mtime) -- shifts the signature and
 * forces a fresh scan, which rewrites the cache. */
#define LIB_CACHE_MAGIC 0x31434d45u /* "EMC1" */

static void sig_walk(const char *dir, int depth, unsigned long *h)
{
	if (depth > 8) return;
	struct stat st;
	if (stat(dir, &st) != 0) return;
	*h = (*h ^ (unsigned long)st.st_mtime) * 1099511628211UL;
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char full[MUS_PATH];
		snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
		struct stat s2;
		if (stat(full, &s2) == 0 && S_ISDIR(s2.st_mode)) {
			for (const char *p = e->d_name; *p; p++) *h = (*h * 131) ^ (unsigned char)*p;
			sig_walk(full, depth + 1, h);
		}
	}
	closedir(d);
}

static unsigned long lib_sig(const char *root)
{
	unsigned long h = 1469598103934665603UL;
	sig_walk(root, 0, &h);
	return h;
}

static void lib_cache_save(const char *path, const mus_library *lib, unsigned long sig)
{
	char tmp[MUS_PATH];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) return;
	uint32_t magic = LIB_CACHE_MAGIC, count = (uint32_t)lib->count;
	uint64_t s64 = sig;
	fwrite(&magic, 4, 1, f);
	fwrite(&s64, 8, 1, f);
	fwrite(&count, 4, 1, f);
	for (int i = 0; i < lib->count; i++) {
		const mus_album *a = &lib->albums[i];
		fwrite(a->name, sizeof a->name, 1, f);
		fwrite(a->artist, sizeof a->artist, 1, f);
		fwrite(a->dir, sizeof a->dir, 1, f);
		fwrite(a->cover, sizeof a->cover, 1, f);
		uint8_t flags = (a->is_singles ? 1 : 0) | (a->branded ? 2 : 0);
		fwrite(&flags, 1, 1, f);
		uint32_t al = a->art_len > 0 ? (uint32_t)a->art_len : 0;
		fwrite(&al, 4, 1, f);
		if (al) fwrite(a->art, 1, al, f);
		uint32_t tc = (uint32_t)a->track_count;
		fwrite(&tc, 4, 1, f);
		for (int j = 0; j < a->track_count; j++) {
			const mus_track *t = &a->tracks[j];
			fwrite(t->name, sizeof t->name, 1, f);
			fwrite(t->path, sizeof t->path, 1, f);
			int32_t tn = t->track_no;
			fwrite(&tn, 4, 1, f);
		}
	}
	if (fclose(f) == 0) rename(tmp, path);
	else remove(tmp);
}

static bool lib_cache_load(const char *path, mus_library *lib, unsigned long expect_sig)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	uint32_t magic = 0, count = 0;
	uint64_t s64 = 0;
	if (fread(&magic, 4, 1, f) != 1 || fread(&s64, 8, 1, f) != 1 || fread(&count, 4, 1, f) != 1 ||
	    magic != LIB_CACHE_MAGIC || s64 != (uint64_t)expect_sig || count > 100000) {
		fclose(f);
		return false;
	}
	memset(lib, 0, sizeof *lib);
	lib->albums = count ? calloc(count, sizeof *lib->albums) : NULL;
	if (count && !lib->albums) { fclose(f); return false; }
	lib->count = (int)count; /* set now so library_free cleans a partial load on failure */
	for (uint32_t i = 0; i < count; i++) {
		mus_album *a = &lib->albums[i];
		if (fread(a->name, sizeof a->name, 1, f) != 1 || fread(a->artist, sizeof a->artist, 1, f) != 1 ||
		    fread(a->dir, sizeof a->dir, 1, f) != 1 || fread(a->cover, sizeof a->cover, 1, f) != 1)
			goto fail;
		uint8_t flags = 0;
		if (fread(&flags, 1, 1, f) != 1) goto fail;
		a->is_singles = (flags & 1) != 0;
		a->branded = (flags & 2) != 0;
		uint32_t al = 0;
		if (fread(&al, 4, 1, f) != 1 || al > 50u * 1024 * 1024) goto fail;
		if (al) {
			a->art = malloc(al);
			if (!a->art) goto fail;
			a->art_len = (int)al;
			if (fread(a->art, 1, al, f) != al) goto fail;
		}
		uint32_t tc = 0;
		if (fread(&tc, 4, 1, f) != 1 || tc > 100000) goto fail;
		if (tc) {
			a->tracks = malloc(tc * sizeof *a->tracks);
			if (!a->tracks) goto fail;
		}
		a->track_count = (int)tc;
		for (uint32_t j = 0; j < tc; j++) {
			mus_track *t = &a->tracks[j];
			if (fread(t->name, sizeof t->name, 1, f) != 1 || fread(t->path, sizeof t->path, 1, f) != 1)
				goto fail;
			int32_t tn = 0;
			if (fread(&tn, 4, 1, f) != 1) goto fail;
			t->track_no = tn;
		}
	}
	fclose(f);
	return true;
fail:
	fclose(f);
	library_free(lib); /* frees albums populated so far; the rest are calloc-zeroed */
	return false;
}

/* Cached whole-library load: reuse the on-disk cache when the tree is unchanged,
 * else do a full scan and refresh the cache. (Callers that scan a single album --
 * e.g. the daemon -- keep using library_scan directly.) */
bool library_scan_cached(const char *root, const char *cache_path, mus_library *lib)
{
	unsigned long sig = lib_sig(root);
	if (lib_cache_load(cache_path, lib, sig)) return lib->count > 0;
	bool ok = library_scan(root, lib);
	if (ok) lib_cache_save(cache_path, lib, sig);
	else remove(cache_path);
	return ok;
}

void library_free(mus_library *lib)
{
	for (int i = 0; i < lib->count; i++) {
		free(lib->albums[i].tracks);
		free(lib->albums[i].art);
	}
	free(lib->albums);
	lib->albums = NULL;
	lib->count = 0;
}
