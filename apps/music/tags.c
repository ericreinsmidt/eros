/* Tag reading for Muse: ID3v2 (mp3) and Vorbis comments (flac/ogg), plus
 * embedded cover art (APIC / FLAC PICTURE / METADATA_BLOCK_PICTURE). Hand-rolled
 * and dependency-free -- only the OGG path leans on the already-linked
 * stb_vorbis for its comment list. Everything is normalized to UTF-8. */
#include "tags.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define STB_VORBIS_HEADER_ONLY
#include "third_party/stb_vorbis.c"

/* text frames beyond this offset in an ID3 tag are ignored while reading text
 * (art usually sits after the text and can be megabytes -- no need to read it
 * just to get the title). Art extraction reads the whole tag instead. */
#define ID3_TEXT_SCAN_CAP (256 * 1024)
#define ID3_MAX_TAG       (32 * 1024 * 1024)

/* ---- small helpers ---- */

static uint32_t be32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* ID3v2 synchsafe: 7 significant bits per byte */
static uint32_t synchsafe(const unsigned char *p)
{
	return (uint32_t)(p[0] & 0x7f) << 21 | (uint32_t)(p[1] & 0x7f) << 14 |
	       (uint32_t)(p[2] & 0x7f) << 7 | (p[3] & 0x7f);
}

static void set_field(char *dst, const char *src)
{
	if (!src || !src[0]) return;
	snprintf(dst, TAG_STR, "%s", src);
}

/* leading integer of a "3" or "3/12" track string */
static int parse_track_no(const char *s)
{
	while (*s == ' ') s++;
	int n = 0;
	if (*s < '0' || *s > '9') return 0;
	while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
	return n;
}

/* ---- text encoding -> UTF-8 ---- */

static void utf8_put(char **o, char *end, unsigned cp)
{
	char *p = *o;
	if (cp < 0x80) { if (p < end) *p++ = (char)cp; }
	else if (cp < 0x800) {
		if (p + 1 < end) { *p++ = (char)(0xc0 | cp >> 6); *p++ = (char)(0x80 | (cp & 0x3f)); }
	} else if (cp < 0x10000) {
		if (p + 2 < end) { *p++ = (char)(0xe0 | cp >> 12); *p++ = (char)(0x80 | (cp >> 6 & 0x3f)); *p++ = (char)(0x80 | (cp & 0x3f)); }
	} else {
		if (p + 3 < end) { *p++ = (char)(0xf0 | cp >> 18); *p++ = (char)(0x80 | (cp >> 12 & 0x3f)); *p++ = (char)(0x80 | (cp >> 6 & 0x3f)); *p++ = (char)(0x80 | (cp & 0x3f)); }
	}
	*o = p;
}

static void latin1_to_utf8(const unsigned char *s, size_t n, char *out)
{
	char *o = out, *end = out + TAG_STR - 1;
	for (size_t i = 0; i < n && s[i]; i++) utf8_put(&o, end, s[i]);
	*o = '\0';
}

static void utf16_to_utf8(const unsigned char *s, size_t n, bool big_endian, char *out)
{
	char *o = out, *end = out + TAG_STR - 1;
	for (size_t i = 0; i + 1 < n; i += 2) {
		unsigned u = big_endian ? (unsigned)s[i] << 8 | s[i + 1]
		                        : (unsigned)s[i + 1] << 8 | s[i];
		if (u == 0) break;
		if (u >= 0xd800 && u <= 0xdbff && i + 3 < n) {   /* surrogate pair */
			unsigned lo = big_endian ? (unsigned)s[i + 2] << 8 | s[i + 3]
			                         : (unsigned)s[i + 3] << 8 | s[i + 2];
			if (lo >= 0xdc00 && lo <= 0xdfff) {
				u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
				i += 2;
			}
		}
		utf8_put(&o, end, u);
	}
	*o = '\0';
}

/* Decode an ID3v2 text-frame payload (leading encoding byte + text) to UTF-8. */
static void id3_text(const unsigned char *d, size_t len, char *out)
{
	out[0] = '\0';
	if (len < 1) return;
	int enc = d[0];
	const unsigned char *t = d + 1;
	size_t n = len - 1;
	switch (enc) {
	case 0: latin1_to_utf8(t, n, out); break;
	case 1:   /* UTF-16 with BOM */
		if (n >= 2 && t[0] == 0xff && t[1] == 0xfe) utf16_to_utf8(t + 2, n - 2, false, out);
		else if (n >= 2 && t[0] == 0xfe && t[1] == 0xff) utf16_to_utf8(t + 2, n - 2, true, out);
		else utf16_to_utf8(t, n, false, out);
		break;
	case 2: utf16_to_utf8(t, n, true, out); break;   /* UTF-16BE, no BOM */
	default: {                                         /* 3 = UTF-8 */
		size_t c = n < TAG_STR - 1 ? n : TAG_STR - 1;
		memcpy(out, t, c); out[c] = '\0';
		char *nul = memchr(out, 0, c);
		(void)nul;
		break;
	}
	}
}

/* ---- base64 (for Vorbis METADATA_BLOCK_PICTURE / COVERART) ---- */

static int b64_val(unsigned char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

static unsigned char *base64_decode(const char *s, int *out_len)
{
	size_t slen = strlen(s);
	unsigned char *out = malloc(slen / 4 * 3 + 4);
	if (!out) return NULL;
	int acc = 0, bits = 0, n = 0;
	for (size_t i = 0; i < slen; i++) {
		int v = b64_val((unsigned char)s[i]);
		if (v < 0) continue;   /* skip '=', whitespace, and any stray byte */
		acc = acc << 6 | v;
		bits += 6;
		if (bits >= 8) { bits -= 8; out[n++] = (unsigned char)(acc >> bits & 0xff); }
	}
	*out_len = n;
	return out;
}

/* ---- ID3v2 (mp3) ---- */

static size_t id3_skip_extended(const unsigned char *buf, size_t size, int ver)
{
	if (size < 4) return 0;
	if (ver == 4) return synchsafe(buf) ;     /* v2.4: size is synchsafe, includes itself */
	return be32(buf) + 4;                      /* v2.3: size excludes the 4-byte length */
}

/* Collapse the 0xFF 0x00 unsynchronisation pairs in place; returns new length. */
static size_t deunsync(unsigned char *buf, size_t n)
{
	size_t w = 0;
	for (size_t r = 0; r < n; r++) {
		buf[w++] = buf[r];
		if (buf[r] == 0xff && r + 1 < n && buf[r + 1] == 0x00) r++;   /* drop the 0x00 */
	}
	return w;
}

/* Find the picture bytes inside an APIC (v2.3/2.4) or PIC (v2.2) frame payload;
 * returns a malloc'd copy. The two frames differ only in the field after the
 * text-encoding byte: APIC has a null-terminated MIME string, PIC a fixed 3-byte
 * image-format code ("JPG"/"PNG"), which is NOT null-terminated. */
static unsigned char *pic_extract(const unsigned char *d, size_t len, int v22, int *out_len)
{
	if (len < 4) return NULL;
	int enc = d[0];
	size_t i = 1;
	if (v22) {
		i += 3;                     /* v2.2 PIC: 3-char image format, not terminated */
	} else {
		while (i < len && d[i]) i++;   /* v2.3/2.4 APIC: null-terminated MIME */
		if (i >= len) return NULL;
		i++;                            /* skip MIME null */
	}
	if (i >= len) return NULL;
	i++;                            /* picture type byte */
	/* description, terminated by null in the frame's text encoding */
	if (enc == 1 || enc == 2) {     /* UTF-16: two-byte null on an even boundary */
		while (i + 1 < len && !(d[i] == 0 && d[i + 1] == 0)) i += 2;
		i += 2;
	} else {
		while (i < len && d[i]) i++;
		i++;
	}
	if (i >= len) return NULL;
	int n = (int)(len - i);
	unsigned char *img = malloc(n);
	if (!img) return NULL;
	memcpy(img, d + i, n);
	*out_len = n;
	return img;
}

static bool id3_parse(unsigned char *buf, size_t size, int ver, int flags,
                      mus_tags *t, unsigned char **art, int *art_len)
{
	if (flags & 0x80) size = deunsync(buf, size);   /* whole-tag unsync (v2.3) */
	size_t pos = 0;
	if (flags & 0x40) pos += id3_skip_extended(buf, size, ver);   /* extended header */

	bool any = false;
	int idlen = ver == 2 ? 3 : 4;   /* v2.2 uses 3-char IDs / 3-byte sizes */
	int hdrlen = ver == 2 ? 6 : 10;
	while (pos + hdrlen <= size && buf[pos]) {
		char id[5] = {0};
		memcpy(id, buf + pos, idlen);
		uint32_t fsize;
		if (ver == 2) fsize = (uint32_t)buf[pos + 3] << 16 | (uint32_t)buf[pos + 4] << 8 | buf[pos + 5];
		else if (ver == 4) fsize = synchsafe(buf + pos + 4);
		else fsize = be32(buf + pos + 4);
		size_t data = pos + hdrlen;
		if (data + fsize > size) break;
		const unsigned char *d = buf + data;

		if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) { id3_text(d, fsize, t->title); any = true; }
		else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) { id3_text(d, fsize, t->artist); any = true; }
		else if (!strcmp(id, "TPE2") || !strcmp(id, "TP2")) {   /* album artist: use only if no TPE1 */
			if (!t->artist[0]) { id3_text(d, fsize, t->artist); any = true; }
		}
		else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) { id3_text(d, fsize, t->album); any = true; }
		else if (!strcmp(id, "TRCK") || !strcmp(id, "TRK")) {
			char tmp[TAG_STR]; id3_text(d, fsize, tmp); t->track_no = parse_track_no(tmp); any = true;
		} else if (art && !*art && (!strcmp(id, "APIC") || !strcmp(id, "PIC"))) {
			*art = pic_extract(d, fsize, !strcmp(id, "PIC"), art_len);
		}
		pos = data + fsize;
	}
	return any;
}

static bool mp3_read(FILE *f, mus_tags *t, unsigned char **art, int *art_len)
{
	unsigned char hdr[10];
	if (fread(hdr, 1, 10, f) != 10 || memcmp(hdr, "ID3", 3) != 0) return false;
	int ver = hdr[3];
	int flags = hdr[5];
	uint32_t size = synchsafe(hdr + 6);
	if (size == 0 || size > ID3_MAX_TAG) return false;

	size_t want = size;
	if (!art && size > ID3_TEXT_SCAN_CAP) want = ID3_TEXT_SCAN_CAP;   /* text-only: cap the read */
	unsigned char *buf = malloc(want);
	if (!buf) return false;
	size_t got = fread(buf, 1, want, f);
	bool ok = id3_parse(buf, got, ver, flags, t, art, art_len);
	free(buf);
	return ok;
}

/* ---- Vorbis comments (shared by flac + ogg) ---- */

static void vorbis_kv(const char *kv, size_t len, mus_tags *t)
{
	const char *eq = memchr(kv, '=', len);
	if (!eq) return;
	size_t klen = (size_t)(eq - kv);
	const char *v = eq + 1;
	size_t vlen = len - klen - 1;
	char val[TAG_STR];
	size_t c = vlen < TAG_STR - 1 ? vlen : TAG_STR - 1;
	memcpy(val, v, c); val[c] = '\0';
	if (klen == 5 && !strncasecmp(kv, "TITLE", 5)) set_field(t->title, val);
	else if (klen == 6 && !strncasecmp(kv, "ARTIST", 6)) set_field(t->artist, val);
	else if (klen == 5 && !strncasecmp(kv, "ALBUM", 5)) set_field(t->album, val);
	else if (klen == 11 && !strncasecmp(kv, "TRACKNUMBER", 11)) t->track_no = parse_track_no(val);
}

/* ---- FLAC ---- */

/* Pull the image bytes out of a FLAC PICTURE metadata block (big-endian). */
static unsigned char *flac_picture_extract(const unsigned char *b, size_t n, int *out_len)
{
	size_t i = 4;                                   /* picture type */
	if (i + 4 > n) return NULL;
	uint32_t mlen = be32(b + i); i += 4 + mlen;     /* mime */
	if (i + 4 > n) return NULL;
	uint32_t dlen = be32(b + i); i += 4 + dlen;     /* description */
	if (i + 16 > n) return NULL;
	i += 16;                                        /* width/height/depth/colors */
	if (i + 4 > n) return NULL;
	uint32_t plen = be32(b + i); i += 4;            /* picture data length */
	if (i + plen > n) return NULL;
	unsigned char *img = malloc(plen ? plen : 1);
	if (!img) return NULL;
	memcpy(img, b + i, plen);
	*out_len = (int)plen;
	return img;
}

static bool flac_read(FILE *f, mus_tags *t, unsigned char **art, int *art_len)
{
	unsigned char magic[4];
	if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "fLaC", 4) != 0) return false;
	bool any = false;
	for (;;) {
		unsigned char h[4];
		if (fread(h, 1, 4, f) != 4) break;
		int last = h[0] & 0x80;
		int type = h[0] & 0x7f;
		uint32_t blen = (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
		if (type == 4) {                                   /* VORBIS_COMMENT */
			unsigned char *b = malloc(blen ? blen : 1);
			if (b && fread(b, 1, blen, f) == blen) {
				/* vendor string, then a list of KEY=VALUE, all little-endian lengths */
				uint32_t p = 0;
				if (blen >= 4) {
					uint32_t vl = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
					p = 4 + vl;
					if (p + 4 <= blen) {
						uint32_t cnt = b[p] | b[p+1] << 8 | b[p+2] << 16 | (uint32_t)b[p+3] << 24;
						p += 4;
						for (uint32_t k = 0; k < cnt && p + 4 <= blen; k++) {
							uint32_t cl = b[p] | b[p+1] << 8 | b[p+2] << 16 | (uint32_t)b[p+3] << 24;
							p += 4;
							if (p + cl > blen) break;
							vorbis_kv((const char *)b + p, cl, t);
							p += cl;
						}
						any = true;
					}
				}
			}
			free(b);
		} else if (type == 6 && art && !*art) {            /* PICTURE */
			unsigned char *b = malloc(blen ? blen : 1);
			if (b && fread(b, 1, blen, f) == blen) *art = flac_picture_extract(b, blen, art_len);
			free(b);
		} else {
			if (fseek(f, (long)blen, SEEK_CUR) != 0) break;
		}
		if (last) break;
	}
	return any;
}

/* ---- OGG (via stb_vorbis's comment list) ---- */

static bool ogg_read(const char *path, mus_tags *t, unsigned char **art, int *art_len)
{
	int err = 0;
	stb_vorbis *v = stb_vorbis_open_filename(path, &err, NULL);
	if (!v) return false;
	stb_vorbis_comment c = stb_vorbis_get_comment(v);
	bool any = false;
	for (int i = 0; i < c.comment_list_length; i++) {
		const char *kv = c.comment_list[i];
		if (!kv) continue;
		size_t len = strlen(kv);
		if (art && !*art && len > 22 && !strncasecmp(kv, "METADATA_BLOCK_PICTURE", 22) && kv[22] == '=') {
			int blen = 0;
			unsigned char *blk = base64_decode(kv + 23, &blen);
			if (blk) { *art = flac_picture_extract(blk, (size_t)blen, art_len); free(blk); }
		} else {
			vorbis_kv(kv, len, t);
			any = true;
		}
	}
	stb_vorbis_close(v);
	return any;
}

/* ---- dispatch ---- */

static int ext_kind(const char *path)   /* 1 mp3, 2 flac, 3 ogg, 0 other */
{
	const char *dot = strrchr(path, '.');
	if (!dot) return 0;
	if (!strcasecmp(dot, ".mp3")) return 1;
	if (!strcasecmp(dot, ".flac")) return 2;
	if (!strcasecmp(dot, ".ogg") || !strcasecmp(dot, ".oga")) return 3;
	return 0;
}

bool tags_read(const char *path, mus_tags *out)
{
	memset(out, 0, sizeof *out);
	int k = ext_kind(path);
	if (k == 3) return ogg_read(path, out, NULL, NULL);   /* stb opens the file itself */
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	bool ok = false;
	if (k == 1) ok = mp3_read(f, out, NULL, NULL);
	else if (k == 2) ok = flac_read(f, out, NULL, NULL);
	fclose(f);
	return ok;
}

unsigned char *tags_read_art(const char *path, int *len)
{
	*len = 0;
	unsigned char *art = NULL;
	int k = ext_kind(path);
	mus_tags scratch;
	memset(&scratch, 0, sizeof scratch);
	if (k == 3) { ogg_read(path, &scratch, &art, len); return art; }
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	if (k == 1) mp3_read(f, &scratch, &art, len);
	else if (k == 2) flac_read(f, &scratch, &art, len);
	fclose(f);
	if (art && *len <= 0) { free(art); art = NULL; }
	return art;
}
