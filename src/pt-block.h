#pragma once
#include <glib.h>

/* One rectangle of a block-element glyph, in unit-cell fractions (0..1).
 * alpha scales the foreground color (shade characters). */
typedef struct { float x, y, w, h; float alpha; } PtBlockRect;

/* Decompose a block-element codepoint (U+2580..U+259F) into up to 4 cell
 * rects. Returns the rect count, or 0 if cp is not handled.
 * Drawing these as exact cell rectangles is what keeps adjacent block cells
 * seamless: the font's glyph ink is narrower than the rounded cell width. */
int pt_block_glyph_rects(guint32 cp, PtBlockRect out[4]);
