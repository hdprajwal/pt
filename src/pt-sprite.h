#pragma once
#include <glib.h>

/* Sprite glyphs: box drawing, block elements and braille, drawn from the cell
 * metrics instead of asked of the font.
 *
 * The font's own box-drawing glyphs are cut for the font's natural line
 * height, which includes line gap. pt's cell height is ascent + descent with
 * no gap, so those glyphs are drawn for a taller cell than they get: at
 * JetBrainsMono Nerd Font 13 the ink of `│` is 27px inside a 24px cell, so
 * every vertical rule pokes out of its row and punches through the horizontal
 * rule above it. Ghostty solves this by never asking the font for these
 * codepoints at all (font/sprite/Face.zig hasCodepoint returns true
 * unconditionally, and CodepointResolver checks the sprite face ahead of every
 * loaded font). This module is the same idea, ported from
 * build/_deps/ghostty-src/src/font/sprite/draw/.
 *
 * Block elements are here for a related but different reason: the font's ink
 * is narrower than the rounded cell width, so adjacent block cells seam.
 *
 * Everything here is pure: no GTK types, no drawing. The caller supplies a
 * sink and turns the primitives into whatever it renders with. */

/* cell_w/cell_h are the cell in widget px. thickness is the base line weight,
 * which is ghostty's box_thickness: the underline thickness, floored at 1
 * (font/Metrics.zig:298,319). Values below 1 are clamped here rather than
 * trusted, since a font that reports a zero underline would otherwise draw
 * nothing at all. */
typedef struct { int cell_w, cell_h, thickness; } PtSpriteMetrics;

/* Path verbs, in cell-local coordinates. Arcs need all three. */
typedef enum { PT_SPRITE_MOVE, PT_SPRITE_LINE, PT_SPRITE_CUBIC } PtSpriteVerb;
typedef struct {
  PtSpriteVerb verb;
  float x[3], y[3];   /* CUBIC: two controls then the endpoint; else [0] only */
} PtSpriteSeg;
typedef struct { PtSpriteSeg seg[8]; int n; } PtSpritePath;

typedef struct {
  /* cell-local widget px, integer; alpha scales fg */
  void (*rect)(void *user, int x, int y, int w, int h, float alpha);
  /* stroked path, butt caps, `width` px */
  void (*stroke)(void *user, const PtSpritePath *path, float width);
} PtSpriteSink;

/* TRUE if this module draws `cp`. Mirrors ghostty's hasCodepoint: the sprite
 * always wins over the font for these, there is no fallback and no config key.
 * Callers use this to decide whether to break a shaped text run, so it has to
 * be cheap and it has to agree exactly with pt_sprite_draw. */
gboolean pt_sprite_has(gunichar cp);

/* Draw `cp` into the sink. Returns FALSE, having made no sink calls at all,
 * for any codepoint this module does not own; the caller then falls through to
 * the font unchanged.
 *
 * Coordinates handed to the sink are cell-local widget px, not device px.
 * Integer coordinates are crisp at integer scale factors; at fractional
 * scaling GTK resamples and this buys alignment rather than perfect
 * crispness. */
gboolean pt_sprite_draw(gunichar cp, const PtSpriteMetrics *m,
                        const PtSpriteSink *sink, void *user);
