#include "pt-sprite.h"
#include <string.h>

/* Span assertions on their own are not enough for this module. A port with the
 * arm-extension conditionals inverted still draws an arm that reaches every
 * edge at the right thickness, because those assertions never look at what
 * happens near the centre. So the tests here work two ways: exact rect lists
 * pinned at ghostty's own four test geometries, and a coverage bitmap the
 * junction properties are read off.
 *
 * The pinned numbers were not derived by hand. They come from running
 * ghostty's own linesChar, dashHorizontal and dashVertical, lifted verbatim
 * out of build/_deps/ghostty-src/src/font/sprite/draw/box.zig into a harness
 * with a stub canvas that prints the boxes it is asked for. The four
 * geometries are the ones ghostty renders its reference PNGs at
 * (sprite/Face.zig:530-533). */

/* ---- recording sink, with a bitmap oracle ---- */

#define REC_PAD 24            /* room for glyphs that draw past the cell */
#define REC_MAX 64            /* largest cell dimension the tests use */
#define REC_SIDE (REC_MAX + 2 * REC_PAD)

typedef struct { int x, y, w, h; float alpha; } RecRect;

typedef struct {
  int cell_w, cell_h;
  int nrect;
  RecRect rect[32];
  int nstroke;
  guint8 px[REC_SIDE * REC_SIDE];
} Rec;

static void rec_rect(void *user, int x, int y, int w, int h, float alpha) {
  Rec *r = user;
  g_assert_cmpint(r->nrect, <, (int)G_N_ELEMENTS(r->rect));
  r->rect[r->nrect++] = (RecRect){ x, y, w, h, alpha };
  for (int j = y; j < y + h; j++) {
    for (int i = x; i < x + w; i++) {
      int gx = i + REC_PAD, gy = j + REC_PAD;
      g_assert_true(gx >= 0 && gx < REC_SIDE && gy >= 0 && gy < REC_SIDE);
      r->px[gy * REC_SIDE + gx] = 1;
    }
  }
}

static void rec_stroke(void *user, const PtSpritePath *path, float width) {
  Rec *r = user;
  (void)path;
  (void)width;
  r->nstroke++;
}

static const PtSpriteSink rec_sink = { rec_rect, rec_stroke };

static gboolean rec_draw(Rec *r, gunichar cp, int cw, int ch, int th) {
  memset(r, 0, sizeof(*r));
  r->cell_w = cw;
  r->cell_h = ch;
  PtSpriteMetrics m = { cw, ch, th };
  return pt_sprite_draw(cp, &m, &rec_sink, r);
}

static gboolean rec_on(const Rec *r, int x, int y) {
  int gx = x + REC_PAD, gy = y + REC_PAD;
  if (gx < 0 || gx >= REC_SIDE || gy < 0 || gy >= REC_SIDE) return FALSE;
  return r->px[gy * REC_SIDE + gx] != 0;
}

/* A run of set pixels. Runs are read inside the cell only, since that is where
 * the joins have to line up. */
typedef struct { int start, len; } Run;

static int rec_runs_row(const Rec *r, int y, Run *out, int max) {
  int n = 0, x = 0;
  while (x < r->cell_w) {
    if (!rec_on(r, x, y)) { x++; continue; }
    int s = x;
    while (x < r->cell_w && rec_on(r, x, y)) x++;
    g_assert_cmpint(n, <, max);
    out[n++] = (Run){ s, x - s };
  }
  return n;
}

static int rec_runs_col(const Rec *r, int x, Run *out, int max) {
  int n = 0, y = 0;
  while (y < r->cell_h) {
    if (!rec_on(r, x, y)) { y++; continue; }
    int s = y;
    while (y < r->cell_h && rec_on(r, x, y)) y++;
    g_assert_cmpint(n, <, max);
    out[n++] = (Run){ s, y - s };
  }
  return n;
}

/* ---- ghostty's four reference geometries ---- */

typedef struct { int w, h, t; } Geom;
static const Geom golden[] = {
  { 18, 36, 4 }, { 12, 24, 3 }, { 11, 21, 2 }, { 9, 17, 1 },
};

/* ---- pinned rect lists ---- */

typedef struct {
  guint32 cp;
  int cw, ch, th;
  int n;
  struct { int x, y, w, h; } box[8];
} Pinned;

/* Every one of these is a junction where the arm-extension logic actually
 * fires: an arm's stop line moves because of what the perpendicular arms are,
 * so getting a conditional backwards changes these and nothing else. */
static const Pinned pinned[] = {
  /* mixed weight: the heavy arm pushes the light arms' stop line out */
  { 0x253d, 18, 36, 4, 4, {{7,0,4,22}, {11,16,7,4}, {7,14,4,22}, {0,14,7,8}} },  /* ┽ */
  { 0x253d, 12, 24, 3, 4, {{4,0,3,15}, {7,10,5,3}, {4,9,3,15}, {0,9,4,6}} },  /* ┽ */
  { 0x253d, 11, 21, 2, 4, {{4,0,2,12}, {6,9,5,2}, {4,8,2,13}, {0,8,4,4}} },  /* ┽ */
  { 0x253d, 9, 17, 1, 4, {{4,0,1,9}, {5,8,4,1}, {4,7,1,10}, {0,7,4,2}} },  /* ┽ */
  { 0x2540, 18, 36, 4, 4, {{5,0,8,16}, {5,16,13,4}, {7,20,4,16}, {0,16,13,4}} },  /* ╀ */
  { 0x2540, 12, 24, 3, 4, {{3,0,6,10}, {3,10,9,3}, {4,13,3,11}, {0,10,9,3}} },  /* ╀ */
  { 0x2540, 11, 21, 2, 4, {{3,0,4,9}, {3,9,8,2}, {4,11,2,10}, {0,9,7,2}} },  /* ╀ */
  { 0x2540, 9, 17, 1, 4, {{3,0,2,8}, {3,8,6,1}, {4,9,1,8}, {0,8,5,1}} },  /* ╀ */
  { 0x2545, 18, 36, 4, 4, {{7,0,4,22}, {5,16,13,4}, {5,14,8,22}, {0,14,13,8}} },  /* ╅ */
  { 0x2545, 12, 24, 3, 4, {{4,0,3,15}, {3,10,9,3}, {3,9,6,15}, {0,9,9,6}} },  /* ╅ */
  { 0x2545, 11, 21, 2, 4, {{4,0,2,12}, {3,9,8,2}, {3,8,4,13}, {0,8,7,4}} },  /* ╅ */
  { 0x2545, 9, 17, 1, 4, {{4,0,1,9}, {3,8,6,1}, {3,7,2,10}, {0,7,5,2}} },  /* ╅ */
  { 0x254a, 18, 36, 4, 4, {{5,0,8,22}, {5,14,13,8}, {5,14,8,22}, {0,16,13,4}} },  /* ╊ */
  { 0x254a, 12, 24, 3, 4, {{3,0,6,15}, {3,9,9,6}, {3,9,6,15}, {0,10,9,3}} },  /* ╊ */
  { 0x254a, 11, 21, 2, 4, {{3,0,4,12}, {3,8,8,4}, {3,8,4,13}, {0,9,7,2}} },  /* ╊ */
  { 0x254a, 9, 17, 1, 4, {{3,0,2,9}, {3,7,6,2}, {3,7,2,10}, {0,8,5,1}} },  /* ╊ */
  /* half-line mixes: one arm light, the other heavy, nothing crossing */
  { 0x257d, 18, 36, 4, 2, {{7,0,4,20}, {5,16,8,20}} },  /* ╽ */
  { 0x257d, 12, 24, 3, 2, {{4,0,3,13}, {3,10,6,14}} },  /* ╽ */
  { 0x257d, 11, 21, 2, 2, {{4,0,2,11}, {3,9,4,12}} },  /* ╽ */
  { 0x257d, 9, 17, 1, 2, {{4,0,1,9}, {3,8,2,9}} },  /* ╽ */
  { 0x257f, 18, 36, 4, 2, {{5,0,8,20}, {7,16,4,20}} },  /* ╿ */
  { 0x257f, 12, 24, 3, 2, {{3,0,6,13}, {4,10,3,14}} },  /* ╿ */
  { 0x257f, 11, 21, 2, 2, {{3,0,4,11}, {4,9,2,12}} },  /* ╿ */
  { 0x257f, 9, 17, 1, 2, {{3,0,2,9}, {4,8,1,9}} },  /* ╿ */
  /* double meets single: the double arm splits around the single one */
  { 0x255e, 18, 36, 4, 4, {{7,0,4,24}, {11,12,7,4}, {11,20,7,4}, {7,12,4,24}} },  /* ╞ */
  { 0x255e, 12, 24, 3, 4, {{4,0,3,16}, {7,7,5,3}, {7,13,5,3}, {4,7,3,17}} },  /* ╞ */
  { 0x255e, 11, 21, 2, 4, {{4,0,2,13}, {6,7,5,2}, {6,11,5,2}, {4,7,2,14}} },  /* ╞ */
  { 0x255e, 9, 17, 1, 4, {{4,0,1,10}, {5,7,4,1}, {5,9,4,1}, {4,7,1,10}} },  /* ╞ */
  { 0x255f, 18, 36, 4, 5, {{3,0,4,20}, {11,0,4,20}, {11,16,7,4}, {3,16,4,20}, {11,16,4,20}} },  /* ╟ */
  { 0x255f, 12, 24, 3, 5, {{1,0,3,13}, {7,0,3,13}, {7,10,5,3}, {1,10,3,14}, {7,10,3,14}} },  /* ╟ */
  { 0x255f, 11, 21, 2, 5, {{2,0,2,11}, {6,0,2,11}, {6,9,5,2}, {2,9,2,12}, {6,9,2,12}} },  /* ╟ */
  { 0x255f, 9, 17, 1, 5, {{3,0,1,9}, {5,0,1,9}, {5,8,4,1}, {3,8,1,9}, {5,8,1,9}} },  /* ╟ */
  { 0x256a, 18, 36, 4, 6, {{7,0,4,24}, {7,12,11,4}, {7,20,11,4}, {7,12,4,24}, {0,12,11,4}, {0,20,11,4}} },  /* ╪ */
  { 0x256a, 12, 24, 3, 6, {{4,0,3,16}, {4,7,8,3}, {4,13,8,3}, {4,7,3,17}, {0,7,7,3}, {0,13,7,3}} },  /* ╪ */
  { 0x256a, 11, 21, 2, 6, {{4,0,2,13}, {4,7,7,2}, {4,11,7,2}, {4,7,2,14}, {0,7,6,2}, {0,11,6,2}} },  /* ╪ */
  { 0x256a, 9, 17, 1, 6, {{4,0,1,10}, {4,7,5,1}, {4,9,5,1}, {4,7,1,10}, {0,7,5,1}, {0,9,5,1}} },  /* ╪ */
  { 0x256b, 18, 36, 4, 6, {{3,0,4,20}, {11,0,4,20}, {3,16,15,4}, {3,16,4,20}, {11,16,4,20}, {0,16,15,4}} },  /* ╫ */
  { 0x256b, 12, 24, 3, 6, {{1,0,3,13}, {7,0,3,13}, {1,10,11,3}, {1,10,3,14}, {7,10,3,14}, {0,10,10,3}} },  /* ╫ */
  { 0x256b, 11, 21, 2, 6, {{2,0,2,11}, {6,0,2,11}, {2,9,9,2}, {2,9,2,12}, {6,9,2,12}, {0,9,8,2}} },  /* ╫ */
  { 0x256b, 9, 17, 1, 6, {{3,0,1,9}, {5,0,1,9}, {3,8,6,1}, {3,8,1,9}, {5,8,1,9}, {0,8,6,1}} },  /* ╫ */
  { 0x256c, 18, 36, 4, 8, {{3,0,4,16}, {11,0,4,16}, {11,12,7,4}, {11,20,7,4}, {3,20,4,16}, {11,20,4,16}, {0,12,7,4}, {0,20,7,4}} },  /* ╬ */
  { 0x256c, 12, 24, 3, 8, {{1,0,3,10}, {7,0,3,10}, {7,7,5,3}, {7,13,5,3}, {1,13,3,11}, {7,13,3,11}, {0,7,4,3}, {0,13,4,3}} },  /* ╬ */
  { 0x256c, 11, 21, 2, 8, {{2,0,2,9}, {6,0,2,9}, {6,7,5,2}, {6,11,5,2}, {2,11,2,10}, {6,11,2,10}, {0,7,4,2}, {0,11,4,2}} },  /* ╬ */
  { 0x256c, 9, 17, 1, 8, {{3,0,1,8}, {5,0,1,8}, {5,7,4,1}, {5,9,4,1}, {3,9,1,8}, {5,9,1,8}, {0,7,4,1}, {0,9,4,1}} },  /* ╬ */
  /* dashes, including the extra pixels handed to dashes not gaps */
  { 0x2504, 18, 36, 4, 3, {{1,16,3,4}, {7,16,3,4}, {13,16,3,4}} },  /* ┄ */
  { 0x2504, 12, 24, 3, 3, {{1,10,2,3}, {5,10,2,3}, {9,10,2,3}} },  /* ┄ */
  { 0x2504, 11, 21, 2, 3, {{0,9,3,2}, {4,9,3,2}, {8,9,2,2}} },  /* ┄ */
  { 0x2504, 9, 17, 1, 3, {{0,8,2,1}, {3,8,2,1}, {6,8,2,1}} },  /* ┄ */
  { 0x2506, 18, 36, 4, 3, {{7,0,4,8}, {7,12,4,8}, {7,24,4,8}} },  /* ┆ */
  { 0x2506, 12, 24, 3, 3, {{4,0,3,4}, {4,8,3,4}, {4,16,3,4}} },  /* ┆ */
  { 0x2506, 11, 21, 2, 3, {{4,0,2,4}, {4,7,2,4}, {4,14,2,4}} },  /* ┆ */
  { 0x2506, 9, 17, 1, 3, {{4,0,1,4}, {4,6,1,4}, {4,12,1,3}} },  /* ┆ */
  { 0x2508, 18, 36, 4, 4, {{1,16,3,4}, {6,16,3,4}, {11,16,2,4}, {15,16,2,4}} },  /* ┈ */
  { 0x2508, 12, 24, 3, 4, {{0,10,2,3}, {3,10,2,3}, {6,10,2,3}, {9,10,2,3}} },  /* ┈ */
  { 0x2508, 11, 21, 2, 4, {{0,9,2,2}, {3,9,2,2}, {6,9,2,2}, {9,9,1,2}} },  /* ┈ */
  { 0x2508, 9, 17, 1, 4, {{0,8,2,1}, {3,8,1,1}, {5,8,1,1}, {7,8,1,1}} },  /* ┈ */
  { 0x254c, 18, 36, 4, 2, {{2,16,5,4}, {11,16,5,4}} },  /* ╌ */
  { 0x254c, 12, 24, 3, 2, {{1,10,3,3}, {7,10,3,3}} },  /* ╌ */
  { 0x254c, 11, 21, 2, 2, {{1,9,4,2}, {7,9,3,2}} },  /* ╌ */
  { 0x254c, 9, 17, 1, 2, {{0,8,4,1}, {5,8,3,1}} },  /* ╌ */
  { 0x254e, 18, 36, 4, 2, {{7,0,4,10}, {7,18,4,10}} },  /* ╎ */
  { 0x254e, 12, 24, 3, 2, {{4,0,3,6}, {4,12,3,6}} },  /* ╎ */
  { 0x254e, 11, 21, 2, 2, {{4,0,2,7}, {4,11,2,6}} },  /* ╎ */
  { 0x254e, 9, 17, 1, 2, {{4,0,1,7}, {4,9,1,6}} },  /* ╎ */
  /* too narrow to dash: ghostty draws a solid light line instead */
  { 0x2504, 5, 10, 1, 1, {{0,4,5,1}} },  /* ┄ */
  { 0x2508, 5, 10, 1, 1, {{0,4,5,1}} },  /* ┈ */
  { 0x2506, 2, 3, 1, 1, {{0,0,1,3}} },  /* ┆ */
  { 0x254e, 2, 3, 1, 1, {{0,0,1,3}} },  /* ╎ */
};

static void test_pinned(void) {
  Rec r;
  for (gsize i = 0; i < G_N_ELEMENTS(pinned); i++) {
    const Pinned *p = &pinned[i];
    if (!rec_draw(&r, p->cp, p->cw, p->ch, p->th))
      g_error("U+%04X %dx%d t%d: not drawn", p->cp, p->cw, p->ch, p->th);
    if (r.nrect != p->n)
      g_error("U+%04X %dx%d t%d: got %d rects want %d", p->cp, p->cw, p->ch,
              p->th, r.nrect, p->n);
    for (int j = 0; j < p->n; j++) {
      if (r.rect[j].x != p->box[j].x || r.rect[j].y != p->box[j].y ||
          r.rect[j].w != p->box[j].w || r.rect[j].h != p->box[j].h)
        g_error("U+%04X %dx%d t%d rect %d: got %d,%d %dx%d want %d,%d %dx%d",
                p->cp, p->cw, p->ch, p->th, j, r.rect[j].x, r.rect[j].y,
                r.rect[j].w, r.rect[j].h, p->box[j].x, p->box[j].y,
                p->box[j].w, p->box[j].h);
      g_assert_cmpfloat(r.rect[j].alpha, ==, 1.0f);
    }
  }
}

/* ---- ╬ ---- */

/* The hardest join in the range: four double arms. The middle of the cell has
 * to stay empty, and the doubled strokes have to be separated by exactly one
 * light stroke at every edge. If the double handling collapses, the middle
 * fills in and the gaps close. */
static void test_double_cross_hole(void) {
  Rec r;
  Run runs[8];
  for (gsize i = 0; i < G_N_ELEMENTS(golden); i++) {
    const Geom *g = &golden[i];
    g_assert_true(rec_draw(&r, 0x256c, g->w, g->h, g->t));

    /* The centre pixel is off, and so is the whole band the four doubled
     * strokes leave between them: ╬ is four corner pieces, not a cross. */
    if (rec_on(&r, g->w / 2, g->h / 2))
      g_error("U+256C %dx%d t%d: centre pixel is on", g->w, g->h, g->t);
    g_assert_cmpint(rec_runs_row(&r, g->h / 2, runs, 8), ==, 0);
    g_assert_cmpint(rec_runs_col(&r, g->w / 2, runs, 8), ==, 0);

    /* The four gaps between the doubled strokes, read at each edge of the
     * cell. Each is exactly one light stroke wide. */
    static const char *where[] = { "top", "bottom", "left", "right" };
    for (int e = 0; e < 4; e++) {
      int n = e == 0   ? rec_runs_row(&r, 0, runs, 8)
              : e == 1 ? rec_runs_row(&r, g->h - 1, runs, 8)
              : e == 2 ? rec_runs_col(&r, 0, runs, 8)
                       : rec_runs_col(&r, g->w - 1, runs, 8);
      if (n != 2)
        g_error("U+256C %dx%d t%d: %s edge has %d runs, want 2", g->w, g->h,
                g->t, where[e], n);
      int gap = runs[1].start - (runs[0].start + runs[0].len);
      if (gap != g->t)
        g_error("U+256C %dx%d t%d: %s edge gap is %d, want %d", g->w, g->h,
                g->t, where[e], gap, g->t);
    }
  }
}

/* ---- every arm reaches its edge ---- */

/* The arm styles ghostty gives each glyph, in up/right/down/left order, from
 * box.zig's own switch. "...." means the glyph is not an intersection glyph:
 * the dashes, the arcs and the diagonals. Kept here as well as in the module
 * so that a mistake packing the styles into bits shows up as a wrong glyph
 * rather than as two matching copies of the same mistake. */
static const char *arms[128] = {
  "nlnl", "nhnh", "lnln", "hnhn", "....", "....", "....", "....",   /* U+2500 */
  "....", "....", "....", "....", "nlln", "nhln", "nlhn", "nhhn",   /* U+2508 */
  "nnll", "nnlh", "nnhl", "nnhh", "llnn", "lhnn", "hlnn", "hhnn",   /* U+2510 */
  "lnnl", "lnnh", "hnnl", "hnnh", "llln", "lhln", "hlln", "llhn",   /* U+2518 */
  "hlhn", "hhln", "lhhn", "hhhn", "lnll", "lnlh", "hnll", "lnhl",   /* U+2520 */
  "hnhl", "hnlh", "lnhh", "hnhh", "nlll", "nllh", "nhll", "nhlh",   /* U+2528 */
  "nlhl", "nlhh", "nhhl", "nhhh", "llnl", "llnh", "lhnl", "lhnh",   /* U+2530 */
  "hlnl", "hlnh", "hhnl", "hhnh", "llll", "lllh", "lhll", "lhlh",   /* U+2538 */
  "hlll", "llhl", "hlhl", "hllh", "hhll", "llhh", "lhhl", "hhlh",   /* U+2540 */
  "lhhh", "hlhh", "hhhl", "hhhh", "....", "....", "....", "....",   /* U+2548 */
  "ndnd", "dndn", "ndln", "nldn", "nddn", "nnld", "nndl", "nndd",   /* U+2550 */
  "ldnn", "dlnn", "ddnn", "lnnd", "dnnl", "dnnd", "ldln", "dldn",   /* U+2558 */
  "dddn", "lnld", "dndl", "dndd", "ndld", "nldl", "nddd", "ldnd",   /* U+2560 */
  "dlnl", "ddnd", "ldld", "dldl", "dddd", "....", "....", "....",   /* U+2568 */
  "....", "....", "....", "....", "nnnl", "lnnn", "nlnn", "nnln",   /* U+2570 */
  "nnnh", "hnnn", "nhnn", "nnhn", "nhnl", "lnhn", "nlnh", "hnln",   /* U+2578 */
};

/* At the cell edge an arm is one run of its own thickness centred on the cell,
 * except a double arm, which is two light runs straddling where the light one
 * would be. Reading this off the bitmap at all four edges catches an arm that
 * stops short, is off centre, or was drawn at the wrong weight. */
static void check_edge(guint32 cp, const Geom *g, char style, int n,
                       const Run *runs, int dim, const char *what) {
  int light = g->t;
  int centre = (dim - light) / 2;
  switch (style) {
    case 'n':
      if (n != 0) g_error("U+%04X %dx%d t%d: %s arm is none but has ink", cp,
                          g->w, g->h, g->t, what);
      break;
    case 'l':
      if (n != 1 || runs[0].start != centre || runs[0].len != light)
        g_error("U+%04X %dx%d t%d: %s light arm %d runs, %d+%d, want 1 run "
                "%d+%d", cp, g->w, g->h, g->t, what, n, n ? runs[0].start : -1,
                n ? runs[0].len : -1, centre, light);
      break;
    case 'h': {
      int heavy = 2 * g->t;
      int hc = (dim - heavy) / 2;
      if (n != 1 || runs[0].start != hc || runs[0].len != heavy)
        g_error("U+%04X %dx%d t%d: %s heavy arm %d runs, %d+%d, want 1 run "
                "%d+%d", cp, g->w, g->h, g->t, what, n, n ? runs[0].start : -1,
                n ? runs[0].len : -1, hc, heavy);
      break;
    }
    case 'd':
      if (n != 2 || runs[0].start != centre - light || runs[0].len != light ||
          runs[1].start != centre + light || runs[1].len != light)
        g_error("U+%04X %dx%d t%d: %s double arm is wrong", cp, g->w, g->h,
                g->t, what);
      break;
    default:
      g_assert_not_reached();
  }
}

static void test_arms_reach_edges(void) {
  Rec r;
  Run runs[8];
  for (gsize i = 0; i < G_N_ELEMENTS(golden); i++) {
    const Geom *g = &golden[i];
    for (guint32 cp = 0x2500; cp <= 0x257f; cp++) {
      const char *a = arms[cp - 0x2500];
      if (a[0] == '.') continue;
      if (!pt_sprite_has(cp)) continue;
      g_assert_true(rec_draw(&r, cp, g->w, g->h, g->t));
      check_edge(cp, g, a[0], rec_runs_row(&r, 0, runs, 8), runs, g->w,
                 "up");
      check_edge(cp, g, a[2], rec_runs_row(&r, g->h - 1, runs, 8), runs,
                 g->w, "down");
      check_edge(cp, g, a[3], rec_runs_col(&r, 0, runs, 8), runs, g->h,
                 "left");
      check_edge(cp, g, a[1], rec_runs_col(&r, g->w - 1, runs, 8), runs,
                 g->h, "right");
    }
  }
}

/* ---- the bug this module exists for ---- */

/* The font's `│` is 27px of ink in a 24px cell, which is what punches through
 * the row above. Nothing drawn here leaves its cell at a usable geometry. */
static void test_ink_fits_the_cell(void) {
  Rec r;
  for (gsize i = 0; i < G_N_ELEMENTS(golden); i++) {
    const Geom *g = &golden[i];
    for (guint32 cp = 0x2500; cp <= 0x257f; cp++) {
      if (!pt_sprite_has(cp)) continue;
      g_assert_true(rec_draw(&r, cp, g->w, g->h, g->t));
      for (int j = 0; j < r.nrect; j++) {
        const RecRect *b = &r.rect[j];
        if (b->x < 0 || b->y < 0 || b->x + b->w > g->w || b->y + b->h > g->h)
          g_error("U+%04X %dx%d t%d: rect %d,%d %dx%d leaves the cell", cp,
                  g->w, g->h, g->t, b->x, b->y, b->w, b->h);
      }
    }
  }
}

/* ---- degenerate metrics ---- */

/* Zig's saturating `-|` is all over the metric arithmetic upstream and C has
 * no equivalent, so a missed clamp shows up as a negative or inverted
 * rectangle once the line is thicker than the cell. */
static void test_saturation(void) {
  static const Geom tiny[] = {
    { 1, 1, 1 }, { 1, 1, 3 }, { 2, 3, 1 }, { 2, 3, 3 }, { 3, 7, 1 },
    { 2, 4, 4 }, { 4, 8, 2 }, { 1, 1, 0 }, { 5, 5, -2 },
  };
  Rec r;
  for (gsize i = 0; i < G_N_ELEMENTS(tiny); i++) {
    const Geom *g = &tiny[i];
    for (guint32 cp = 0x2500; cp <= 0x257f; cp++) {
      if (!pt_sprite_has(cp)) continue;
      g_assert_true(rec_draw(&r, cp, g->w, g->h, g->t));
      for (int j = 0; j < r.nrect; j++) {
        const RecRect *b = &r.rect[j];
        if (b->w <= 0 || b->h <= 0 || b->x < 0 || b->y < 0)
          g_error("U+%04X %dx%d t%d: rect %d,%d %dx%d", cp, g->w, g->h, g->t,
                  b->x, b->y, b->w, b->h);
      }
    }
  }
}

/* ---- coverage ---- */

static void test_coverage(void) {
  Rec r;
  for (guint32 cp = 0x2500; cp <= 0x257f; cp++) {
    gboolean drew = rec_draw(&r, cp, 12, 24, 3);
    if (drew != pt_sprite_has(cp))
      g_error("U+%04X: has() says %d, draw() says %d", cp, pt_sprite_has(cp),
              drew);
    if (drew && r.nrect == 0 && r.nstroke == 0)
      g_error("U+%04X: claimed but drew nothing", cp);
  }
}

static void test_not_ours(void) {
  static const gunichar other[] = { 0, 'A', 0x24ff, 0x2580, 0x2588, 0x259f,
                                    0x2600, 0x28ff, 0xe0b0 };
  Rec r;
  for (gsize i = 0; i < G_N_ELEMENTS(other); i++) {
    if (pt_sprite_has(other[i])) continue;
    g_assert_false(rec_draw(&r, other[i], 12, 24, 3));
    /* A FALSE return has to leave the sink untouched: the caller falls
     * through to the font and anything drawn here would double up. */
    if (r.nrect != 0 || r.nstroke != 0)
      g_error("U+%04X: not ours but emitted %d rects and %d strokes",
              other[i], r.nrect, r.nstroke);
  }
}

int main(void) {
  test_pinned();
  test_double_cross_hole();
  test_arms_reach_edges();
  test_ink_fits_the_cell();
  test_saturation();
  test_coverage();
  test_not_ours();
  g_print("test-sprite: OK\n");
  return 0;
}
