#ifndef EROS_TEXT_H
#define EROS_TEXT_H

#include <SDL_ttf.h>

/* Y at which to draw a TTF text surface so its CAP height is vertically centered
 * within [row_top, row_top + row_h].
 *
 * A TTF surface spans the font's full ascent + descent, and capital letters sit
 * just under the ascent with no descenders -- so top-aligning a surface (or
 * centering the whole surface) leaves the visible text riding high in its row.
 * Center on the caps instead, using the top of 'H' relative to the baseline.
 *
 * Shared by EROS's SDL apps (the launcher and Muse). The in-game menu (minarch,
 * a separate NextUI-derived binary) carries its own identical copy. */
static inline int EROS_textCenterY(TTF_Font *f, int row_top, int row_h)
{
	int cap_top = 0;
	TTF_GlyphMetrics(f, 'H', NULL, NULL, NULL, &cap_top, NULL);
	return row_top + (row_h + cap_top) / 2 - TTF_FontAscent(f);
}

#endif /* EROS_TEXT_H */
