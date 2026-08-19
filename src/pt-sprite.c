#include "pt-sprite.h"

/* Ported from build/_deps/ghostty-src/src/font/sprite/draw/{common,box}.zig.
 *
 * Zig's saturating operators `-|` and `+|` are all over the metric arithmetic
 * there, on unsigned types. C has no equivalent, so every `-|` becomes an
 * explicit sat_sub. Without that, a degenerate cell (thicker line than cell)
 * sends the coordinates negative and the glyph inverts. `+|` needs nothing:
 * these are small pixel counts in int and cannot wrap.
 *
 * Unlike ghostty there is no canvas and no clip here, so drawing outside the
 * cell is free. Ghostty budgets overhang with cell/4 padding on each edge
 * (sprite/Face.zig:200-215) because it rasterizes into an atlas region;
 * pt appends nodes straight into the snapshot, so the diagonals just draw past
 * the cell edge and land on the neighbour. This is the one place pt is simpler
 * than ghostty rather than sloppier. */

/* Line weights. Ghostty's Thickness.height (common.zig:33-39). super_light is
 * unused by the box range but kept for the ranges that do use it. */
static int thick_light(int base) { return base; }
static int thick_heavy(int base) { return base * 2; }

/* Zig's `-|` on unsigned. */
static int sat_sub(int a, int b) { return a > b ? a - b : 0; }

/* Canvas.box: two corners, normalized, filled. Ghostty's Box.rect takes the
 * min/max of the two points, so a reversed pair draws the same rectangle
 * rather than nothing (canvas.zig:23-42). Zero-area boxes paint no pixels
 * there, so they emit no sink call here. */
static void emit_box(const PtSpriteSink *sink, void *user,
                     int x0, int y0, int x1, int y1) {
  int x = MIN(x0, x1), y = MIN(y0, y1);
  int w = MAX(x0, x1) - x, h = MAX(y0, y1) - y;
  if (w <= 0 || h <= 0) return;
  sink->rect(user, x, y, w, h, 1.0f);
}

/* Horizontal line with its top edge at `y`, between `x1` and `x2`. */
static void hline(const PtSpriteSink *sink, void *user,
                  int x1, int x2, int y, int thick_px) {
  emit_box(sink, user, x1, y, x2, y + thick_px);
}

/* Vertical line with its left edge at `x`, between `y1` and `y2`. */
static void vline(const PtSpriteSink *sink, void *user,
                  int y1, int y2, int x, int thick_px) {
  emit_box(sink, user, x, y1, x + thick_px, y2);
}

static void hline_middle(const PtSpriteMetrics *m, const PtSpriteSink *sink,
                         void *user, int thick_px) {
  hline(sink, user, 0, m->cell_w, sat_sub(m->cell_h, thick_px) / 2, thick_px);
}

static void vline_middle(const PtSpriteMetrics *m, const PtSpriteSink *sink,
                         void *user, int thick_px) {
  vline(sink, user, 0, m->cell_h, sat_sub(m->cell_w, thick_px) / 2, thick_px);
}

/* ---- intersection glyphs ---- */

/* One arm's style, packed two bits per direction to match ghostty's
 * `Lines` packed struct (box.zig:34-46) field for field. */
#define LN_NONE   0
#define LN_LIGHT  1
#define LN_HEAVY  2
#define LN_DOUBLE 3

#define ARM_UP(p)    ((p) & 3)
#define ARM_RIGHT(p) (((p) >> 2) & 3)
#define ARM_DOWN(p)  (((p) >> 4) & 3)
#define ARM_LEFT(p)  (((p) >> 6) & 3)

/* box.zig linesChar (:398-636). The four `*_bottom`/`*_top`/`*_right`/`*_left`
 * expressions in the middle are the whole point of this function: an arm stops
 * short of, or reaches past, the centre depending on what the *perpendicular*
 * arms are doing, which is what makes ┽ and ╪ and ╬ join instead of blob. */
static void lines_char(const PtSpriteMetrics *m, const PtSpriteSink *sink,
                       void *user, guint8 packed) {
  const int up = ARM_UP(packed), right = ARM_RIGHT(packed);
  const int down = ARM_DOWN(packed), left = ARM_LEFT(packed);

  const int light_px = thick_light(m->thickness);
  const int heavy_px = thick_heavy(m->thickness);

  /* Top and bottom of light horizontal strokes */
  const int h_light_top = sat_sub(m->cell_h, light_px) / 2;
  const int h_light_bottom = h_light_top + light_px;
  /* Top and bottom of heavy horizontal strokes */
  const int h_heavy_top = sat_sub(m->cell_h, heavy_px) / 2;
  const int h_heavy_bottom = h_heavy_top + heavy_px;
  /* Outer edges of the doubled horizontal pair; the inner edges are the light
   * stroke's own edges, so the gap between the two is light_px wide. */
  const int h_double_top = sat_sub(h_light_top, light_px);
  const int h_double_bottom = h_light_bottom + light_px;

  /* Left and right of light vertical strokes */
  const int v_light_left = sat_sub(m->cell_w, light_px) / 2;
  const int v_light_right = v_light_left + light_px;
  /* Left and right of heavy vertical strokes */
  const int v_heavy_left = sat_sub(m->cell_w, heavy_px) / 2;
  const int v_heavy_right = v_heavy_left + heavy_px;
  /* Outer edges of the doubled vertical pair */
  const int v_double_left = sat_sub(v_light_left, light_px);
  const int v_double_right = v_light_right + light_px;

  /* The bottom of the up line */
  const int up_bottom =
      (left == LN_HEAVY || right == LN_HEAVY) ? h_heavy_bottom
      : (left != right || down == up)
          ? ((left == LN_DOUBLE || right == LN_DOUBLE) ? h_double_bottom
                                                       : h_light_bottom)
      : (left == LN_NONE && right == LN_NONE) ? h_light_bottom
                                              : h_light_top;

  /* The top of the down line */
  const int down_top =
      (left == LN_HEAVY || right == LN_HEAVY) ? h_heavy_top
      : (left != right || up == down)
          ? ((left == LN_DOUBLE || right == LN_DOUBLE) ? h_double_top
                                                       : h_light_top)
      : (left == LN_NONE && right == LN_NONE) ? h_light_top
                                              : h_light_bottom;

  /* The right of the left line */
  const int left_right =
      (up == LN_HEAVY || down == LN_HEAVY) ? v_heavy_right
      : (up != down || left == right)
          ? ((up == LN_DOUBLE || down == LN_DOUBLE) ? v_double_right
                                                    : v_light_right)
      : (up == LN_NONE && down == LN_NONE) ? v_light_right
                                           : v_light_left;

  /* The left of the right line */
  const int right_left =
      (up == LN_HEAVY || down == LN_HEAVY) ? v_heavy_left
      : (up != down || right == left)
          ? ((up == LN_DOUBLE || down == LN_DOUBLE) ? v_double_left
                                                    : v_light_left)
      : (up == LN_NONE && down == LN_NONE) ? v_light_left
                                           : v_light_right;

  switch (up) {
    case LN_NONE: break;
    case LN_LIGHT:
      emit_box(sink, user, v_light_left, 0, v_light_right, up_bottom);
      break;
    case LN_HEAVY:
      emit_box(sink, user, v_heavy_left, 0, v_heavy_right, up_bottom);
      break;
    default: {
      int left_bottom = (left == LN_DOUBLE) ? h_light_top : up_bottom;
      int right_bottom = (right == LN_DOUBLE) ? h_light_top : up_bottom;
      emit_box(sink, user, v_double_left, 0, v_light_left, left_bottom);
      emit_box(sink, user, v_light_right, 0, v_double_right, right_bottom);
      break;
    }
  }

  switch (right) {
    case LN_NONE: break;
    case LN_LIGHT:
      emit_box(sink, user, right_left, h_light_top, m->cell_w, h_light_bottom);
      break;
    case LN_HEAVY:
      emit_box(sink, user, right_left, h_heavy_top, m->cell_w, h_heavy_bottom);
      break;
    default: {
      int top_left = (up == LN_DOUBLE) ? v_light_right : right_left;
      int bottom_left = (down == LN_DOUBLE) ? v_light_right : right_left;
      emit_box(sink, user, top_left, h_double_top, m->cell_w, h_light_top);
      emit_box(sink, user, bottom_left, h_light_bottom, m->cell_w,
               h_double_bottom);
      break;
    }
  }

  switch (down) {
    case LN_NONE: break;
    case LN_LIGHT:
      emit_box(sink, user, v_light_left, down_top, v_light_right, m->cell_h);
      break;
    case LN_HEAVY:
      emit_box(sink, user, v_heavy_left, down_top, v_heavy_right, m->cell_h);
      break;
    default: {
      int left_top = (left == LN_DOUBLE) ? h_light_bottom : down_top;
      int right_top = (right == LN_DOUBLE) ? h_light_bottom : down_top;
      emit_box(sink, user, v_double_left, left_top, v_light_left, m->cell_h);
      emit_box(sink, user, v_light_right, right_top, v_double_right, m->cell_h);
      break;
    }
  }

  switch (left) {
    case LN_NONE: break;
    case LN_LIGHT:
      emit_box(sink, user, 0, h_light_top, left_right, h_light_bottom);
      break;
    case LN_HEAVY:
      emit_box(sink, user, 0, h_heavy_top, left_right, h_heavy_bottom);
      break;
    default: {
      int top_right = (up == LN_DOUBLE) ? v_light_left : left_right;
      int bottom_right = (down == LN_DOUBLE) ? v_light_left : left_right;
      emit_box(sink, user, 0, h_double_top, top_right, h_light_top);
      emit_box(sink, user, 0, h_light_bottom, bottom_right, h_double_bottom);
      break;
    }
  }
}

/* ---- dashed lines ---- */

/* box.zig dashHorizontal (:779-856). The dashes have to tile: a run of ┄ next
 * to each other has to read as one broken line with even gaps, so there is
 * half a gap at each end rather than a whole one, and the pixels that do not
 * divide evenly go into the dashes rather than the gaps, where they are less
 * obvious. */
static void dash_h(const PtSpriteMetrics *m, const PtSpriteSink *sink,
                   void *user, int count, int thick_px, int desired_gap) {
  /* For N dashes there are N-1 gaps between them plus the two half gaps at the
   * ends, so N gaps in total. */
  const int gap_count = count;

  /* Below one pixel per dash and per gap the pattern cannot be drawn at all,
   * so fall back to a solid light line. */
  if (m->cell_w < count + gap_count) {
    hline_middle(m, sink, user, thick_light(m->thickness));
    return;
  }

  /* Gaps never take more than half the cell, or the dashes get too small to
   * read as a line. */
  const int gap_width = MIN(desired_gap, m->cell_w / (2 * count));
  const int total_gap_width = gap_count * gap_width;
  const int total_dash_width = m->cell_w - total_gap_width;
  const int dash_width = total_dash_width / count;
  int extra = total_dash_width % count;

  const int y = sat_sub(m->cell_h, thick_px) / 2;
  int x = gap_width / 2;   /* half a gap in from the left edge */

  for (int i = 0; i < count; i++) {
    int x1 = x + dash_width;
    if (extra > 0) { extra--; x1++; }
    hline(sink, user, x, x1, y, thick_px);
    x = x1 + gap_width;
  }
}

/* box.zig dashVertical (:858-928). Same idea as dash_h with one difference
 * ghostty calls out: the leftover gap goes entirely at the bottom rather than
 * being split across both ends. A whole gap joins to a solid neighbour better
 * than two half gaps do, and vertical centering matters much less visually. */
static void dash_v(const PtSpriteMetrics *m, const PtSpriteSink *sink,
                   void *user, int count, int thick_px, int desired_gap) {
  const int gap_count = count;

  if (m->cell_h < count + gap_count) {
    vline_middle(m, sink, user, thick_light(m->thickness));
    return;
  }

  const int gap_height = MIN(desired_gap, m->cell_h / (2 * count));
  const int total_gap_height = gap_count * gap_height;
  const int total_dash_height = m->cell_h - total_gap_height;
  const int dash_height = total_dash_height / count;
  int extra = total_dash_height % count;

  const int x = sat_sub(m->cell_w, thick_px) / 2;
  int y = 0;   /* starts flush with the top of the cell */

  for (int i = 0; i < count; i++) {
    int y1 = y + dash_height;
    if (extra > 0) { extra--; y1++; }
    vline(sink, user, y, y1, x, thick_px);
    y = y1 + gap_height;
  }
}

/* ---- arcs and diagonals ---- */

/* The only shapes in this range that are not rectangles. Everything else here
 * is a pixel loop; these two need a real curve and a real slope, so they go to
 * the sink's stroke instead, butt caps, one width. */

static void path_move(PtSpritePath *p, double x, double y) {
  p->seg[p->n].verb = PT_SPRITE_MOVE;
  p->seg[p->n].x[0] = (float)x;
  p->seg[p->n].y[0] = (float)y;
  p->n++;
}

static void path_line(PtSpritePath *p, double x, double y) {
  p->seg[p->n].verb = PT_SPRITE_LINE;
  p->seg[p->n].x[0] = (float)x;
  p->seg[p->n].y[0] = (float)y;
  p->n++;
}

static void path_cubic(PtSpritePath *p, double c1x, double c1y, double c2x,
                       double c2y, double x, double y) {
  p->seg[p->n].verb = PT_SPRITE_CUBIC;
  p->seg[p->n].x[0] = (float)c1x;
  p->seg[p->n].y[0] = (float)c1y;
  p->seg[p->n].x[1] = (float)c2x;
  p->seg[p->n].y[1] = (float)c2y;
  p->seg[p->n].x[2] = (float)x;
  p->seg[p->n].y[2] = (float)y;
  p->n++;
}

typedef enum { CORNER_TL, CORNER_TR, CORNER_BL, CORNER_BR } SpriteCorner;

/* box.zig arc (:694-776). Not an arc of a circle end to end: a straight
 * lead-in from the cell edge to where the circle starts, one cubic across the
 * quarter turn, then a straight lead-out to the other edge. The controls sit a
 * quarter of the radius away from the centre line, which is ghostty's own
 * constant, not a circle-fitting one. `corner` names the pair of edges the
 * curve joins, so ╭ is the bottom-right corner. */
static void arc(const PtSpriteMetrics *m, const PtSpriteSink *sink, void *user,
                SpriteCorner corner) {
  const int thick_px = thick_light(m->thickness);
  const double fw = m->cell_w, fh = m->cell_h, ft = thick_px;
  /* The integer division happens before the half-thickness is added, the same
   * way the rectangles are centred, so a curve lines up with the straight
   * stroke it continues. */
  const double cx = (double)(sat_sub(m->cell_w, thick_px) / 2) + ft / 2;
  const double cy = (double)(sat_sub(m->cell_h, thick_px) / 2) + ft / 2;
  const double r = MIN(fw, fh) / 2;
  const double s = 0.25;   /* how far from the centre the controls sit */

  PtSpritePath p = { { { 0, { 0 }, { 0 } } }, 0 };
  switch (corner) {
    case CORNER_TL:
      path_move(&p, cx, 0);
      path_line(&p, cx, cy - r);
      path_cubic(&p, cx, cy - s * r, cx - s * r, cy, cx - r, cy);
      path_line(&p, 0, cy);
      break;
    case CORNER_TR:
      path_move(&p, cx, 0);
      path_line(&p, cx, cy - r);
      path_cubic(&p, cx, cy - s * r, cx + s * r, cy, cx + r, cy);
      path_line(&p, fw, cy);
      break;
    case CORNER_BL:
      path_move(&p, cx, fh);
      path_line(&p, cx, cy + r);
      path_cubic(&p, cx, cy + s * r, cx - s * r, cy, cx - r, cy);
      path_line(&p, 0, cy);
      break;
    case CORNER_BR:
      path_move(&p, cx, fh);
      path_line(&p, cx, cy + r);
      path_cubic(&p, cx, cy + s * r, cx + s * r, cy, cx + r, cy);
      path_line(&p, fw, cy);
      break;
  }
  sink->stroke(user, &p, (float)ft);
}

/* box.zig lightDiagonalUpperRightToLowerLeft and its mirror (:638-684). Both
 * deliberately overshoot the cell corners by half a slope step so that a run
 * of them chains into one unbroken line instead of showing a nick at every
 * cell boundary. pt has no clip, so the overshoot simply lands on the
 * neighbouring cell. */
static void diagonal(const PtSpriteMetrics *m, const PtSpriteSink *sink,
                     void *user, gboolean upper_right_to_lower_left) {
  const double fw = m->cell_w, fh = m->cell_h;
  const double slope_x = MIN(1.0, fw / fh);
  const double slope_y = MIN(1.0, fh / fw);

  PtSpritePath p = { { { 0, { 0 }, { 0 } } }, 0 };
  if (upper_right_to_lower_left) {
    path_move(&p, fw + 0.5 * slope_x, -0.5 * slope_y);
    path_line(&p, -0.5 * slope_x, fh + 0.5 * slope_y);
  } else {
    path_move(&p, -0.5 * slope_x, -0.5 * slope_y);
    path_line(&p, fw + 0.5 * slope_x, fh + 0.5 * slope_y);
  }
  sink->stroke(user, &p, (float)thick_light(m->thickness));
}

/* ---- dispatch ---- */

/* The arm styles for every intersection glyph in U+2500..U+257F, transcribed
 * from box.zig's own switch (:58-393). 0 means the codepoint is not drawn by
 * lines_char and the switch in pt_sprite_draw handles it: the dashes, the arcs
 * and the diagonals. No real glyph has all four arms none, so 0 is free to be
 * the sentinel. */
#define N LN_NONE
#define L LN_LIGHT
#define H LN_HEAVY
#define D LN_DOUBLE
#define LN(up, right, down, left) \
  (guint8)((up) | ((right) << 2) | ((down) << 4) | ((left) << 6))

static const guint8 box_lines[0x80] = {
  /* U+2500 ─ */ LN(N, L, N, L),
  /* U+2501 ━ */ LN(N, H, N, H),
  /* U+2502 │ */ LN(L, N, L, N),
  /* U+2503 ┃ */ LN(H, N, H, N),
  /* U+2504 ┄ */ 0,  /* not a lines char */
  /* U+2505 ┅ */ 0,  /* not a lines char */
  /* U+2506 ┆ */ 0,  /* not a lines char */
  /* U+2507 ┇ */ 0,  /* not a lines char */
  /* U+2508 ┈ */ 0,  /* not a lines char */
  /* U+2509 ┉ */ 0,  /* not a lines char */
  /* U+250A ┊ */ 0,  /* not a lines char */
  /* U+250B ┋ */ 0,  /* not a lines char */
  /* U+250C ┌ */ LN(N, L, L, N),
  /* U+250D ┍ */ LN(N, H, L, N),
  /* U+250E ┎ */ LN(N, L, H, N),
  /* U+250F ┏ */ LN(N, H, H, N),
  /* U+2510 ┐ */ LN(N, N, L, L),
  /* U+2511 ┑ */ LN(N, N, L, H),
  /* U+2512 ┒ */ LN(N, N, H, L),
  /* U+2513 ┓ */ LN(N, N, H, H),
  /* U+2514 └ */ LN(L, L, N, N),
  /* U+2515 ┕ */ LN(L, H, N, N),
  /* U+2516 ┖ */ LN(H, L, N, N),
  /* U+2517 ┗ */ LN(H, H, N, N),
  /* U+2518 ┘ */ LN(L, N, N, L),
  /* U+2519 ┙ */ LN(L, N, N, H),
  /* U+251A ┚ */ LN(H, N, N, L),
  /* U+251B ┛ */ LN(H, N, N, H),
  /* U+251C ├ */ LN(L, L, L, N),
  /* U+251D ┝ */ LN(L, H, L, N),
  /* U+251E ┞ */ LN(H, L, L, N),
  /* U+251F ┟ */ LN(L, L, H, N),
  /* U+2520 ┠ */ LN(H, L, H, N),
  /* U+2521 ┡ */ LN(H, H, L, N),
  /* U+2522 ┢ */ LN(L, H, H, N),
  /* U+2523 ┣ */ LN(H, H, H, N),
  /* U+2524 ┤ */ LN(L, N, L, L),
  /* U+2525 ┥ */ LN(L, N, L, H),
  /* U+2526 ┦ */ LN(H, N, L, L),
  /* U+2527 ┧ */ LN(L, N, H, L),
  /* U+2528 ┨ */ LN(H, N, H, L),
  /* U+2529 ┩ */ LN(H, N, L, H),
  /* U+252A ┪ */ LN(L, N, H, H),
  /* U+252B ┫ */ LN(H, N, H, H),
  /* U+252C ┬ */ LN(N, L, L, L),
  /* U+252D ┭ */ LN(N, L, L, H),
  /* U+252E ┮ */ LN(N, H, L, L),
  /* U+252F ┯ */ LN(N, H, L, H),
  /* U+2530 ┰ */ LN(N, L, H, L),
  /* U+2531 ┱ */ LN(N, L, H, H),
  /* U+2532 ┲ */ LN(N, H, H, L),
  /* U+2533 ┳ */ LN(N, H, H, H),
  /* U+2534 ┴ */ LN(L, L, N, L),
  /* U+2535 ┵ */ LN(L, L, N, H),
  /* U+2536 ┶ */ LN(L, H, N, L),
  /* U+2537 ┷ */ LN(L, H, N, H),
  /* U+2538 ┸ */ LN(H, L, N, L),
  /* U+2539 ┹ */ LN(H, L, N, H),
  /* U+253A ┺ */ LN(H, H, N, L),
  /* U+253B ┻ */ LN(H, H, N, H),
  /* U+253C ┼ */ LN(L, L, L, L),
  /* U+253D ┽ */ LN(L, L, L, H),
  /* U+253E ┾ */ LN(L, H, L, L),
  /* U+253F ┿ */ LN(L, H, L, H),
  /* U+2540 ╀ */ LN(H, L, L, L),
  /* U+2541 ╁ */ LN(L, L, H, L),
  /* U+2542 ╂ */ LN(H, L, H, L),
  /* U+2543 ╃ */ LN(H, L, L, H),
  /* U+2544 ╄ */ LN(H, H, L, L),
  /* U+2545 ╅ */ LN(L, L, H, H),
  /* U+2546 ╆ */ LN(L, H, H, L),
  /* U+2547 ╇ */ LN(H, H, L, H),
  /* U+2548 ╈ */ LN(L, H, H, H),
  /* U+2549 ╉ */ LN(H, L, H, H),
  /* U+254A ╊ */ LN(H, H, H, L),
  /* U+254B ╋ */ LN(H, H, H, H),
  /* U+254C ╌ */ 0,  /* not a lines char */
  /* U+254D ╍ */ 0,  /* not a lines char */
  /* U+254E ╎ */ 0,  /* not a lines char */
  /* U+254F ╏ */ 0,  /* not a lines char */
  /* U+2550 ═ */ LN(N, D, N, D),
  /* U+2551 ║ */ LN(D, N, D, N),
  /* U+2552 ╒ */ LN(N, D, L, N),
  /* U+2553 ╓ */ LN(N, L, D, N),
  /* U+2554 ╔ */ LN(N, D, D, N),
  /* U+2555 ╕ */ LN(N, N, L, D),
  /* U+2556 ╖ */ LN(N, N, D, L),
  /* U+2557 ╗ */ LN(N, N, D, D),
  /* U+2558 ╘ */ LN(L, D, N, N),
  /* U+2559 ╙ */ LN(D, L, N, N),
  /* U+255A ╚ */ LN(D, D, N, N),
  /* U+255B ╛ */ LN(L, N, N, D),
  /* U+255C ╜ */ LN(D, N, N, L),
  /* U+255D ╝ */ LN(D, N, N, D),
  /* U+255E ╞ */ LN(L, D, L, N),
  /* U+255F ╟ */ LN(D, L, D, N),
  /* U+2560 ╠ */ LN(D, D, D, N),
  /* U+2561 ╡ */ LN(L, N, L, D),
  /* U+2562 ╢ */ LN(D, N, D, L),
  /* U+2563 ╣ */ LN(D, N, D, D),
  /* U+2564 ╤ */ LN(N, D, L, D),
  /* U+2565 ╥ */ LN(N, L, D, L),
  /* U+2566 ╦ */ LN(N, D, D, D),
  /* U+2567 ╧ */ LN(L, D, N, D),
  /* U+2568 ╨ */ LN(D, L, N, L),
  /* U+2569 ╩ */ LN(D, D, N, D),
  /* U+256A ╪ */ LN(L, D, L, D),
  /* U+256B ╫ */ LN(D, L, D, L),
  /* U+256C ╬ */ LN(D, D, D, D),
  /* U+256D ╭ */ 0,  /* not a lines char */
  /* U+256E ╮ */ 0,  /* not a lines char */
  /* U+256F ╯ */ 0,  /* not a lines char */
  /* U+2570 ╰ */ 0,  /* not a lines char */
  /* U+2571 ╱ */ 0,  /* not a lines char */
  /* U+2572 ╲ */ 0,  /* not a lines char */
  /* U+2573 ╳ */ 0,  /* not a lines char */
  /* U+2574 ╴ */ LN(N, N, N, L),
  /* U+2575 ╵ */ LN(L, N, N, N),
  /* U+2576 ╶ */ LN(N, L, N, N),
  /* U+2577 ╷ */ LN(N, N, L, N),
  /* U+2578 ╸ */ LN(N, N, N, H),
  /* U+2579 ╹ */ LN(H, N, N, N),
  /* U+257A ╺ */ LN(N, H, N, N),
  /* U+257B ╻ */ LN(N, N, H, N),
  /* U+257C ╼ */ LN(N, H, N, L),
  /* U+257D ╽ */ LN(L, N, H, N),
  /* U+257E ╾ */ LN(N, L, N, H),
  /* U+257F ╿ */ LN(H, N, L, N),
};

#undef N
#undef L
#undef H
#undef D
#undef LN

gboolean pt_sprite_has(gunichar cp) {
  return cp >= 0x2500 && cp <= 0x257f;
}

gboolean pt_sprite_draw(gunichar cp, const PtSpriteMetrics *m,
                        const PtSpriteSink *sink, void *user) {
  if (!pt_sprite_has(cp)) return FALSE;

  /* Ghostty floors box_thickness at 1 where it derives it from the face
   * (Metrics.zig:298). pt's underline thickness comes straight from Pango, so
   * the floor lives here. */
  PtSpriteMetrics mm = *m;
  if (mm.thickness < 1) mm.thickness = 1;
  const int light = thick_light(mm.thickness);
  const int heavy = thick_heavy(mm.thickness);
  /* The dotted and dashed forms want a gap of at least 4px, or the line reads
   * as solid; the closer-spaced U+254C..U+254F ask for a thickness-sized gap
   * instead. Both are ghostty's literals. */
  const int wide_gap = MAX(4, light);

  switch (cp) {
    case 0x2504: dash_h(&mm, sink, user, 3, light, wide_gap); return TRUE;
    case 0x2505: dash_h(&mm, sink, user, 3, heavy, wide_gap); return TRUE;
    case 0x2506: dash_v(&mm, sink, user, 3, light, wide_gap); return TRUE;
    case 0x2507: dash_v(&mm, sink, user, 3, heavy, wide_gap); return TRUE;
    case 0x2508: dash_h(&mm, sink, user, 4, light, wide_gap); return TRUE;
    case 0x2509: dash_h(&mm, sink, user, 4, heavy, wide_gap); return TRUE;
    case 0x250a: dash_v(&mm, sink, user, 4, light, wide_gap); return TRUE;
    case 0x250b: dash_v(&mm, sink, user, 4, heavy, wide_gap); return TRUE;
    case 0x254c: dash_h(&mm, sink, user, 2, light, light); return TRUE;
    case 0x254d: dash_h(&mm, sink, user, 2, heavy, heavy); return TRUE;
    case 0x254e: dash_v(&mm, sink, user, 2, light, heavy); return TRUE;
    case 0x254f: dash_v(&mm, sink, user, 2, heavy, heavy); return TRUE;
    case 0x256d: arc(&mm, sink, user, CORNER_BR); return TRUE;
    case 0x256e: arc(&mm, sink, user, CORNER_BL); return TRUE;
    case 0x256f: arc(&mm, sink, user, CORNER_TL); return TRUE;
    case 0x2570: arc(&mm, sink, user, CORNER_TR); return TRUE;
    case 0x2571: diagonal(&mm, sink, user, TRUE); return TRUE;
    case 0x2572: diagonal(&mm, sink, user, FALSE); return TRUE;
    case 0x2573:
      diagonal(&mm, sink, user, TRUE);
      diagonal(&mm, sink, user, FALSE);
      return TRUE;
    default:
      lines_char(&mm, sink, user, box_lines[cp - 0x2500]);
      return TRUE;
  }
}
