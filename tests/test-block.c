#include "pt-block.h"

static void assert_rect(const PtBlockRect *r, float x, float y, float w,
                        float h, float alpha) {
  g_assert_cmpfloat(r->x, ==, x);
  g_assert_cmpfloat(r->y, ==, y);
  g_assert_cmpfloat(r->w, ==, w);
  g_assert_cmpfloat(r->h, ==, h);
  g_assert_cmpfloat(r->alpha, ==, alpha);
}

static void test_full_block(void) {
  PtBlockRect r[4];
  g_assert_cmpint(pt_block_glyph_rects(0x2588, r), ==, 1);
  assert_rect(&r[0], 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
}

static void test_halves(void) {
  PtBlockRect r[4];
  g_assert_cmpint(pt_block_glyph_rects(0x2580, r), ==, 1);
  assert_rect(&r[0], 0.0f, 0.0f, 1.0f, 0.5f, 1.0f);
  g_assert_cmpint(pt_block_glyph_rects(0x2584, r), ==, 1);
  assert_rect(&r[0], 0.0f, 0.5f, 1.0f, 0.5f, 1.0f);
  g_assert_cmpint(pt_block_glyph_rects(0x258c, r), ==, 1);
  assert_rect(&r[0], 0.0f, 0.0f, 0.5f, 1.0f, 1.0f);
  g_assert_cmpint(pt_block_glyph_rects(0x2590, r), ==, 1);
  assert_rect(&r[0], 0.5f, 0.0f, 0.5f, 1.0f, 1.0f);
}

static void test_eighths(void) {
  PtBlockRect r[4];
  g_assert_cmpint(pt_block_glyph_rects(0x2581, r), ==, 1);
  assert_rect(&r[0], 0.0f, 7.0f / 8.0f, 1.0f, 1.0f / 8.0f, 1.0f);
  g_assert_cmpint(pt_block_glyph_rects(0x2582, r), ==, 1);
  assert_rect(&r[0], 0.0f, 6.0f / 8.0f, 1.0f, 2.0f / 8.0f, 1.0f);
  g_assert_cmpint(pt_block_glyph_rects(0x258f, r), ==, 1);
  assert_rect(&r[0], 0.0f, 0.0f, 1.0f / 8.0f, 1.0f, 1.0f);
  g_assert_cmpint(pt_block_glyph_rects(0x2589, r), ==, 1);
  assert_rect(&r[0], 0.0f, 0.0f, 7.0f / 8.0f, 1.0f, 1.0f);
  g_assert_cmpint(pt_block_glyph_rects(0x2594, r), ==, 1);
  assert_rect(&r[0], 0.0f, 0.0f, 1.0f, 1.0f / 8.0f, 1.0f);
  g_assert_cmpint(pt_block_glyph_rects(0x2595, r), ==, 1);
  assert_rect(&r[0], 7.0f / 8.0f, 0.0f, 1.0f / 8.0f, 1.0f, 1.0f);
}

static void test_shades(void) {
  PtBlockRect r[4];
  g_assert_cmpint(pt_block_glyph_rects(0x2591, r), ==, 1);
  assert_rect(&r[0], 0.0f, 0.0f, 1.0f, 1.0f, 0.25f);
  g_assert_cmpint(pt_block_glyph_rects(0x2592, r), ==, 1);
  assert_rect(&r[0], 0.0f, 0.0f, 1.0f, 1.0f, 0.5f);
  g_assert_cmpint(pt_block_glyph_rects(0x2593, r), ==, 1);
  assert_rect(&r[0], 0.0f, 0.0f, 1.0f, 1.0f, 0.75f);
}

static void test_quadrants(void) {
  struct { guint32 cp; int filled; } expect[] = {
    { 0x2596, 1 }, { 0x2597, 1 }, { 0x2598, 1 }, { 0x2599, 3 },
    { 0x259a, 2 }, { 0x259b, 3 }, { 0x259c, 3 }, { 0x259d, 1 },
    { 0x259e, 2 }, { 0x259f, 3 },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(expect); i++) {
    PtBlockRect r[4];
    int n = pt_block_glyph_rects(expect[i].cp, r);
    if (n != expect[i].filled)
      g_error("U+%04X: got %d rects want %d", expect[i].cp, n,
              expect[i].filled);
    float area = 0;
    for (int j = 0; j < n; j++) {
      g_assert_cmpfloat(r[j].w, ==, 0.5f);
      g_assert_cmpfloat(r[j].h, ==, 0.5f);
      g_assert_true(r[j].x == 0.0f || r[j].x == 0.5f);
      g_assert_true(r[j].y == 0.0f || r[j].y == 0.5f);
      g_assert_cmpfloat(r[j].alpha, ==, 1.0f);
      area += r[j].w * r[j].h;
    }
    g_assert_cmpfloat(area, ==, expect[i].filled * 0.25f);
  }
  /* the quadrants each glyph fills, as (x, y) pairs, checked exhaustively:
   * a mask typo would otherwise pass the area check above */
  PtBlockRect r[4];
  g_assert_cmpint(pt_block_glyph_rects(0x2596, r), ==, 1);
  assert_rect(&r[0], 0.0f, 0.5f, 0.5f, 0.5f, 1.0f);
  g_assert_cmpint(pt_block_glyph_rects(0x2597, r), ==, 1);
  assert_rect(&r[0], 0.5f, 0.5f, 0.5f, 0.5f, 1.0f);
  g_assert_cmpint(pt_block_glyph_rects(0x2598, r), ==, 1);
  assert_rect(&r[0], 0.0f, 0.0f, 0.5f, 0.5f, 1.0f);
  g_assert_cmpint(pt_block_glyph_rects(0x259d, r), ==, 1);
  assert_rect(&r[0], 0.5f, 0.0f, 0.5f, 0.5f, 1.0f);
  g_assert_cmpint(pt_block_glyph_rects(0x259a, r), ==, 2);
  assert_rect(&r[0], 0.0f, 0.0f, 0.5f, 0.5f, 1.0f);
  assert_rect(&r[1], 0.5f, 0.5f, 0.5f, 0.5f, 1.0f);
  g_assert_cmpint(pt_block_glyph_rects(0x259e, r), ==, 2);
  assert_rect(&r[0], 0.5f, 0.0f, 0.5f, 0.5f, 1.0f);
  assert_rect(&r[1], 0.0f, 0.5f, 0.5f, 0.5f, 1.0f);
  g_assert_cmpint(pt_block_glyph_rects(0x2599, r), ==, 3);
  assert_rect(&r[0], 0.0f, 0.0f, 0.5f, 0.5f, 1.0f);
  assert_rect(&r[1], 0.0f, 0.5f, 0.5f, 0.5f, 1.0f);
  assert_rect(&r[2], 0.5f, 0.5f, 0.5f, 0.5f, 1.0f);
  g_assert_cmpint(pt_block_glyph_rects(0x259b, r), ==, 3);
  assert_rect(&r[0], 0.0f, 0.0f, 0.5f, 0.5f, 1.0f);
  assert_rect(&r[1], 0.5f, 0.0f, 0.5f, 0.5f, 1.0f);
  assert_rect(&r[2], 0.0f, 0.5f, 0.5f, 0.5f, 1.0f);
  g_assert_cmpint(pt_block_glyph_rects(0x259c, r), ==, 3);
  assert_rect(&r[0], 0.0f, 0.0f, 0.5f, 0.5f, 1.0f);
  assert_rect(&r[1], 0.5f, 0.0f, 0.5f, 0.5f, 1.0f);
  assert_rect(&r[2], 0.5f, 0.5f, 0.5f, 0.5f, 1.0f);
  g_assert_cmpint(pt_block_glyph_rects(0x259f, r), ==, 3);
  assert_rect(&r[0], 0.5f, 0.0f, 0.5f, 0.5f, 1.0f);
  assert_rect(&r[1], 0.0f, 0.5f, 0.5f, 0.5f, 1.0f);
  assert_rect(&r[2], 0.5f, 0.5f, 0.5f, 0.5f, 1.0f);
}

static void test_range_sweep(void) {
  for (guint32 cp = 0x2580; cp <= 0x259f; cp++) {
    PtBlockRect r[4];
    int n = pt_block_glyph_rects(cp, r);
    if (n < 1 || n > 4)
      g_error("U+%04X: got %d rects", cp, n);
    for (int i = 0; i < n; i++) {
      g_assert_cmpfloat(r[i].x, >=, 0.0f);
      g_assert_cmpfloat(r[i].y, >=, 0.0f);
      g_assert_cmpfloat(r[i].w, >, 0.0f);
      g_assert_cmpfloat(r[i].h, >, 0.0f);
      g_assert_cmpfloat(r[i].x + r[i].w, <=, 1.0f);
      g_assert_cmpfloat(r[i].y + r[i].h, <=, 1.0f);
      g_assert_cmpfloat(r[i].alpha, >, 0.0f);
      g_assert_cmpfloat(r[i].alpha, <=, 1.0f);
    }
  }
}

static void test_not_handled(void) {
  PtBlockRect r[4];
  g_assert_cmpint(pt_block_glyph_rects('A', r), ==, 0);
  g_assert_cmpint(pt_block_glyph_rects(0x2500, r), ==, 0);
  g_assert_cmpint(pt_block_glyph_rects(0x257f, r), ==, 0);
  g_assert_cmpint(pt_block_glyph_rects(0x25a0, r), ==, 0);
  g_assert_cmpint(pt_block_glyph_rects(0, r), ==, 0);
}

int main(void) {
  test_full_block();
  test_halves();
  test_eighths();
  test_shades();
  test_quadrants();
  test_range_sweep();
  test_not_handled();
  g_print("test-block: OK\n");
  return 0;
}
