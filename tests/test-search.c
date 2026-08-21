#include "pt-search.h"
#include "pt-term-core.h"
#include "pt-config.h"
#include <string.h>

/* The rows a test matches against are built cell by cell, exactly the way
 * pt_term_core_search_rows builds them: each cell contributes its UTF-8
 * bytes (case-folded here too, so the fixtures exercise the same per-cell
 * folding the extraction does) and one map entry per byte, all pointing at
 * that cell's column. A helper takes (text, width) pairs and hands back one
 * PtSearchRows row. */

typedef struct { const char *text; int width; } TestCell;

static void build_row(const TestCell *cells, int n, char **text_out,
                      GArray **map_out) {
  GString *text = g_string_new(NULL);
  GArray *map = g_array_new(FALSE, FALSE, sizeof(guint16));
  for (int i = 0; i < n; i++) {
    gchar *folded = g_utf8_casefold(cells[i].text, -1);
    guint16 col = (guint16)i;
    for (const char *p = folded; *p != '\0'; p++)
      g_array_append_val(map, col);
    g_string_append_len(text, folded, -1);
    g_free(folded);
    /* Wide characters own two cells; the spacer tail carries no byte, so
     * the fixture skips it the way the extraction does. */
    if (cells[i].width == 2) i++;
  }
  *text_out = g_string_free(text, FALSE);
  *map_out = map;
}

static void build_grid(const TestCell **rows, const int *counts, int n_rows,
                       PtSearchRows *out) {
  memset(out, 0, sizeof *out);
  out->n_rows = n_rows;
  out->rows = g_new0(char *, (gsize)n_rows);
  out->maps = g_new0(GArray *, (gsize)n_rows);
  for (int i = 0; i < n_rows; i++)
    build_row(rows[i], counts[i], &out->rows[i], &out->maps[i]);
}

/* One ASCII row from plain text: every byte is its own cell. */
static void ascii_row(const char *s, TestCell *out, int *count) {
  int n = 0;
  for (const char *p = s; *p != '\0'; p = g_utf8_next_char(p), n++) {
    gsize len = (gsize)(g_utf8_next_char(p) - p);
    char *ch = g_strndup(p, len);
    out[n].text = ch;
    out[n].width = 1;
  }
  *count = n;
}

static int count_matches(GArray *m) { return m != NULL ? (int)m->len : 0; }

static gboolean match_at(GArray *m, int i, int row, int sc, int ec) {
  if (m == NULL || i >= (int)m->len) return FALSE;
  PtSearchMatch *mm = &g_array_index(m, PtSearchMatch, i);
  return mm->row == row && mm->start_col == sc && mm->end_col == ec;
}

/* ---- basic matching ---- */
static void test_basic(void) {
  TestCell r0[64], r1[64];
  int n0, n1;
  ascii_row("hello world", r0, &n0);
  ascii_row("world", r1, &n1);
  const TestCell *rows[] = { r0, r1 };
  int counts[] = { n0, n1 };
  PtSearchRows grid;
  build_grid(rows, counts, 2, &grid);

  GArray *m = pt_search_find((const char *const *)grid.rows,
                             (const GArray *const *)grid.maps, 2, "world");
  g_assert_cmpint(count_matches(m), ==, 2);
  g_assert_true(match_at(m, 0, 0, 6, 11));
  g_assert_true(match_at(m, 1, 1, 0, 5));
  g_array_unref(m);

  /* No hit at all is still an array, just an empty one. */
  GArray *none = pt_search_find((const char *const *)grid.rows,
                                (const GArray *const *)grid.maps, 2, "xyz");
  g_assert_nonnull(none);
  g_assert_cmpint(count_matches(none), ==, 0);
  g_array_unref(none);

  pt_search_rows_clear(&grid);
  for (int i = 0; i < n0; i++) g_free((gpointer)r0[i].text);
  for (int i = 0; i < n1; i++) g_free((gpointer)r1[i].text);
}

/* ---- case folding, including ß which grows under folding ---- */
static void test_case_folding(void) {
  /* "straße" folds per cell to s t r a s s e — seven folded bytes over six
   * cells, both of ß's bytes naming column 4. The uppercase needle folds
   * whole on the caller's side of the boundary. */
  TestCell cells[] = {
    { "s", 1 }, { "t", 1 }, { "r", 1 }, { "a", 1 },
    { "ß", 1 }, { "e", 1 },
  };
  const TestCell *rows[] = { cells };
  int counts[] = { 6 };
  PtSearchRows grid;
  build_grid(rows, counts, 1, &grid);

  GArray *m = pt_search_find((const char *const *)grid.rows,
                             (const GArray *const *)grid.maps, 1, "STRASSE");
  g_assert_cmpint(count_matches(m), ==, 1);
  g_assert_true(match_at(m, 0, 0, 0, 6));
  g_array_unref(m);

  /* And the reverse direction of the same folding. */
  GArray *lower = pt_search_find((const char *const *)grid.rows,
                                 (const GArray *const *)grid.maps, 1, "Straße");
  g_assert_cmpint(count_matches(lower), ==, 1);
  g_array_unref(lower);

  pt_search_rows_clear(&grid);
}

/* ---- unicode needle: CJK ---- */
static void test_unicode_needle(void) {
  /* Each character is one fixture cell here; the matcher only ever sees
     text and map, so real terminal widths are /search/wide-columns' job. */
  TestCell cells[16];
  int n;
  ascii_row("こんにちは世界、また世界", cells, &n);
  const TestCell *rows[] = { cells };
  int counts[] = { n };
  PtSearchRows grid;
  build_grid(rows, counts, 1, &grid);

  GArray *m = pt_search_find((const char *const *)grid.rows,
                             (const GArray *const *)grid.maps, 1, "世界");
  g_assert_cmpint(count_matches(m), ==, 2);
  g_assert_true(match_at(m, 0, 0, 5, 7));
  g_assert_true(match_at(m, 1, 0, 10, 12));
  g_array_unref(m);

  pt_search_rows_clear(&grid);
  for (int i = 0; i < n; i++) g_free((gpointer)cells[i].text);
}

/* ---- empty needle ---- */
static void test_empty_needle(void) {
  TestCell cells[8];
  int n;
  ascii_row("abc", cells, &n);
  const TestCell *rows[] = { cells };
  int counts[] = { n };
  PtSearchRows grid;
  build_grid(rows, counts, 1, &grid);

  GArray *m1 = pt_search_find((const char *const *)grid.rows,
                              (const GArray *const *)grid.maps, 1, "");
  g_assert_cmpint(count_matches(m1), ==, 0);
  g_array_unref(m1);
  GArray *m2 = pt_search_find((const char *const *)grid.rows,
                              (const GArray *const *)grid.maps, 1, NULL);
  g_assert_cmpint(count_matches(m2), ==, 0);
  g_array_unref(m2);

  pt_search_rows_clear(&grid);
  for (int i = 0; i < n; i++) g_free((gpointer)cells[i].text);
}

/* ---- multiple matches per row ---- */
static void test_multiple_per_row(void) {
  TestCell cells[16];
  int n;
  ascii_row("abXabXab", cells, &n);
  const TestCell *rows[] = { cells };
  int counts[] = { n };
  PtSearchRows grid;
  build_grid(rows, counts, 1, &grid);

  GArray *m = pt_search_find((const char *const *)grid.rows,
                             (const GArray *const *)grid.maps, 1, "ab");
  g_assert_cmpint(count_matches(m), ==, 3);
  g_assert_true(match_at(m, 0, 0, 0, 2));
  g_assert_true(match_at(m, 1, 0, 3, 5));
  g_assert_true(match_at(m, 2, 0, 6, 8));

  /* Overlaps do not double-count: "aa" in "aaa" is one match. */
  TestCell run[4];
  int rn;
  ascii_row("aaa", run, &rn);
  const TestCell *rrows[] = { run };
  int rcounts[] = { rn };
  PtSearchRows rgrid;
  build_grid(rrows, rcounts, 1, &rgrid);
  GArray *ov = pt_search_find((const char *const *)rgrid.rows,
                              (const GArray *const *)rgrid.maps, 1, "aa");
  g_assert_cmpint(count_matches(ov), ==, 1);
  g_assert_true(match_at(ov, 0, 0, 0, 2));
  g_array_unref(ov);

  pt_search_rows_clear(&grid);
  pt_search_rows_clear(&rgrid);
  for (int i = 0; i < n; i++) g_free((gpointer)cells[i].text);
  for (int i = 0; i < rn; i++) g_free((gpointer)run[i].text);
}

/* ---- wide-character column mapping ---- */
static void test_wide_columns(void) {
  /* a 界 b: three cells of text plus the spacer tail after 界. The folded
   * text reads "a界b" — five bytes — and the map must say byte 0 came from
   * column 0, bytes 1..3 from column 1, byte 4 from column 3 (the spacer's
   * column 2 never appears). */
  TestCell cells[] = {
    { "a", 1 }, { "界", 2 }, { "", 1 },   /* "" = spacer tail, skipped */
    { "b", 1 },
  };
  const TestCell *rows[] = { cells };
  int counts[] = { 3 };                 /* the spacer is not a cell */
  /* build_row skips width-2 tails itself, so hand-assemble instead. */
  GString *text = g_string_new(NULL);
  GArray *map = g_array_new(FALSE, FALSE, sizeof(guint16));
  struct { const char *t; guint16 col; } spec[] = {
    { "a", 0 }, { "界", 1 }, { "b", 3 },
  };
  for (guint i = 0; i < G_N_ELEMENTS(spec); i++) {
    guint16 col = spec[i].col;
    for (const char *p = spec[i].t; *p != '\0'; p++)
      g_array_append_val(map, col);
    g_string_append(text, spec[i].t);
  }
  g_assert_cmpint((int)text->len, ==, 5);

  GArray *m = pt_search_find(
      (const char *const[]){ text->str },
      (const GArray *const[]){ map }, 1, "a界");
  g_assert_cmpint(count_matches(m), ==, 1);
  /* start_col 0 through end_col 2 covers the wide character's own head
   * cell; drawing (end - start) = 2 cells wide paints head + spacer. */
  g_assert_true(match_at(m, 0, 0, 0, 2));
  g_array_unref(m);

  /* Matching the tail side lands on the right column too. */
  GArray *m2 = pt_search_find(
      (const char *const[]){ text->str },
      (const GArray *const[]){ map }, 1, "界b");
  g_assert_cmpint(count_matches(m2), ==, 1);
  g_assert_true(match_at(m2, 0, 0, 1, 4));
  g_array_unref(m2);

  g_array_unref(map);
  g_string_free(text, TRUE);
}

/* ---- stepping, including wrap in both directions ---- */
static GArray *matches_at_rows(const int *rws, int n) {
  GArray *m = g_array_new(FALSE, FALSE, sizeof(PtSearchMatch));
  for (int i = 0; i < n; i++) {
    PtSearchMatch mm = { .row = rws[i], .start_col = 0, .end_col = 1 };
    g_array_append_val(m, mm);
  }
  return m;
}

static void test_step(void) {
  int rws[] = { 2, 5, 9 };
  GArray *m = matches_at_rows(rws, 3);

  /* Plain walking wraps past both ends. */
  g_assert_cmpint(pt_search_step(m, 0, +1, 24, 0), ==, 1);
  g_assert_cmpint(pt_search_step(m, 2, +1, 24, 0), ==, 0);
  g_assert_cmpint(pt_search_step(m, 0, -1, 24, 0), ==, 2);
  g_assert_cmpint(pt_search_step(m, 2, -1, 24, 0), ==, 1);

  /* No current match: Enter starts at the first match from the viewport
     down, Shift+Enter at the last one above it. */
  g_assert_cmpint(pt_search_step(m, -1, +1, 24, 0), ==, 0);
  g_assert_cmpint(pt_search_step(m, -1, +1, 24, 3), ==, 1);
  g_assert_cmpint(pt_search_step(m, -1, +1, 24, 7), ==, 2);
  g_assert_cmpint(pt_search_step(m, -1, +1, 24, 20), ==, 0);  /* wrap */
  g_assert_cmpint(pt_search_step(m, -1, -1, 24, 7), ==, 1);
  g_assert_cmpint(pt_search_step(m, -1, -1, 24, 2), ==, 2);
  g_assert_cmpint(pt_search_step(m, -1, -1, 24, 1), ==, 2);   /* wrap */

  /* Nothing to walk answers -1 whatever is asked. */
  g_assert_cmpint(pt_search_step(NULL, 0, +1, 24, 0), ==, -1);
  GArray *empty = g_array_new(FALSE, FALSE, sizeof(PtSearchMatch));
  g_assert_cmpint(pt_search_step(empty, -1, +1, 24, 0), ==, -1);
  g_array_unref(empty);

  g_array_unref(m);
}

/* ---- live extraction ----
 *
 * Everything above feeds the matcher by hand; these drive the real thing.
 * They spawn a shell like tests/test-term-core.c does, let output land,
 * then ask for the extracted rows and check the payload's invariants. */

typedef struct { GMainLoop *loop; const char *marker; const char *wait_for;
                 gboolean found; } LiveCtx;

static void live_on_draw(PtTermCore *core, gpointer user) {
  LiveCtx *ctx = user;
  pt_term_core_sync(core);
  char *text = pt_term_core_grid_text(core);
  if (text != NULL && strstr(text, ctx->wait_for) != NULL)
    ctx->found = TRUE;
  g_free(text);
  if (ctx->found) g_main_loop_quit(ctx->loop);
}

static PtTermCore *live_run(const char *cmd, LiveCtx *ctx) {
  const char *argv[] = { "/bin/sh", "-c", cmd, NULL };
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16,
                                      PT_CONFIG_SCROLLBACK_LIMIT_DEFAULT,
                                      &err);
  g_assert_no_error(err);
  PtTermCoreCallbacks cbs = { .draw = live_on_draw };
  pt_term_core_set_callbacks(core, &cbs, ctx);
  return core;
}

static gboolean live_timeout(gpointer user) {
  g_main_loop_quit(((LiveCtx *)user)->loop);
  return G_SOURCE_REMOVE;
}

/* Wait until `marker` shows up in the visible grid (or time out). */
static void live_wait(PtTermCore *core, LiveCtx *ctx) {
  guint to = g_timeout_add_seconds(10, live_timeout, ctx);
  g_main_loop_run(ctx->loop);
  g_source_remove(to);
  g_assert_true(ctx->found);
  pt_term_core_sync(core);
}

static void test_extraction_basic(void) {
  LiveCtx ctx = { .loop = g_main_loop_new(NULL, FALSE),
                  .marker = "search-marker-nine",
                  .wait_for = "search-marker-nine" };
  PtTermCore *core = live_run("printf 'line-one\\nsearch-marker-nine\\n'; sleep 30",
                              &ctx);
  live_wait(core, &ctx);

  PtSearchRows rows;
  g_assert_true(pt_term_core_search_rows(core, &rows));
  g_assert_cmpint(rows.n_rows, >=, 24);

  /* Find the marker through the real payload and confirm the columns it
     reports point at the right cells of their row. */
  GArray *m = pt_search_find((const char *const *)rows.rows,
                             (const GArray *const *)rows.maps,
                             rows.n_rows, ctx.marker);
  g_assert_cmpint(count_matches(m), ==, 1);
  PtSearchMatch *mm = &g_array_index(m, PtSearchMatch, 0);
  g_assert_cmpint(mm->end_col - mm->start_col, ==,
                  (int)strlen(ctx.marker));
  /* The matched slice really is the marker: its first byte's map entry
     names the match's own start column. */
  const char *at = strstr(rows.rows[mm->row], ctx.marker);
  g_assert_nonnull(at);
  gsize byte = (gsize)(at - rows.rows[mm->row]);
  g_assert_cmpuint(g_array_index(rows.maps[mm->row], guint16, byte), ==,
                   (guint16)mm->start_col);
  g_array_unref(m);

  /* Map length tracks text length on every row: the lockstep invariant. */
  for (int i = 0; i < rows.n_rows; i++)
    g_assert_cmpuint(rows.maps[i]->len, ==, strlen(rows.rows[i]));

  pt_search_rows_clear(&rows);
  pt_term_core_free(core);
  g_main_loop_unref(ctx.loop);
}

static void test_extraction_scrollback(void) {
  LiveCtx ctx = { .loop = g_main_loop_new(NULL, FALSE),
                  .marker = "anchor-deep-history",
                  .wait_for = "filler 199" };
  /* The anchor is printed first and then pushed into history by two
     hundred lines of filler, so a hit on it proves the search reached
     past the viewport. */
  char *cmd = g_strdup_printf(
      "echo '%s'; i=0; while [ $i -lt 200 ]; do echo \"filler $i\"; "
      "i=$((i+1)); done; sleep 30", ctx.marker);
  const char *argv[] = { "/bin/sh", "-c", cmd, NULL };
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16,
                                      PT_CONFIG_SCROLLBACK_LIMIT_DEFAULT,
                                      &err);
  g_assert_no_error(err);
  g_free(cmd);
  PtTermCoreCallbacks cbs = { .draw = live_on_draw };
  pt_term_core_set_callbacks(core, &cbs, &ctx);
  live_wait(core, &ctx);

  PtSearchRows rows;
  g_assert_true(pt_term_core_search_rows(core, &rows));
  g_assert_cmpint(rows.n_rows, >, 24);   /* scrollback actually exists */

  GArray *m = pt_search_find((const char *const *)rows.rows,
                             (const GArray *const *)rows.maps,
                             rows.n_rows, ctx.marker);
  g_assert_cmpint(count_matches(m), ==, 1);
  PtSearchMatch *mm = &g_array_index(m, PtSearchMatch, 0);
  /* The hit sits in history: at least one viewport's worth of rows below
     it. */
  g_assert_cmpint(mm->row, <=, rows.n_rows - 25);
  g_array_unref(m);

  pt_search_rows_clear(&rows);
  pt_term_core_free(core);
  g_main_loop_unref(ctx.loop);
}

static void test_extraction_wide_chars(void) {
  LiveCtx ctx = { .loop = g_main_loop_new(NULL, FALSE),
                  .marker = "wide",
                  .wait_for = "wide" };
  /* x 界 y: one wide character mid-row. printf writes the UTF-8 directly. */
  char *cmd = g_strdup_printf(
      "printf 'wide x\\xe7\\x95\\x8cy end\\n'; sleep 30");
  const char *argv[] = { "/bin/sh", "-c", cmd, NULL };
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16,
                                      PT_CONFIG_SCROLLBACK_LIMIT_DEFAULT,
                                      &err);
  g_assert_no_error(err);
  g_free(cmd);
  PtTermCoreCallbacks cbs = { .draw = live_on_draw };
  pt_term_core_set_callbacks(core, &cbs, &ctx);
  live_wait(core, &ctx);

  PtSearchRows rows;
  g_assert_true(pt_term_core_search_rows(core, &rows));

  /* Locate the printed row by its distinctive prefix. */
  int row = -1;
  for (int i = 0; i < rows.n_rows; i++)
    if (strncmp(rows.rows[i], "wide x", 6) == 0) { row = i; break; }
  g_assert_cmpint(row, >=, 0);
  const char *text = rows.rows[row];
  GArray *map = rows.maps[row];

  /* Byte layout: "wide x" is 6 bytes (cols 0..5), 界 is 3 bytes (all col 6),
     y is col 8 — column 7 was the spacer tail and carries no byte. */
  g_assert_cmpuint(strlen(text), ==, map->len);
  g_assert_cmpuint(g_array_index(map, guint16, 6), ==, 6);
  g_assert_cmpuint(g_array_index(map, guint16, 7), ==, 6);
  g_assert_cmpuint(g_array_index(map, guint16, 8), ==, 6);
  g_assert_cmpuint(g_array_index(map, guint16, 9), ==, 8);

  /* And a search across the wide character lands on the right cells. */
  GArray *m = pt_search_find((const char *const *)rows.rows,
                             (const GArray *const *)rows.maps,
                             rows.n_rows, "x界y");
  g_assert_cmpint(count_matches(m), ==, 1);
  PtSearchMatch *mm = &g_array_index(m, PtSearchMatch, 0);
  g_assert_cmpint(mm->row, ==, row);
  g_assert_cmpint(mm->start_col, ==, 5);
  g_assert_cmpint(mm->end_col, ==, 9);
  g_array_unref(m);

  pt_search_rows_clear(&rows);
  pt_term_core_free(core);
  g_main_loop_unref(ctx.loop);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/search/basic", test_basic);
  g_test_add_func("/search/case-folding", test_case_folding);
  g_test_add_func("/search/unicode-needle", test_unicode_needle);
  g_test_add_func("/search/empty-needle", test_empty_needle);
  g_test_add_func("/search/multiple-per-row", test_multiple_per_row);
  g_test_add_func("/search/wide-columns", test_wide_columns);
  g_test_add_func("/search/step", test_step);
  g_test_add_func("/search/extraction-basic", test_extraction_basic);
  g_test_add_func("/search/extraction-scrollback", test_extraction_scrollback);
  g_test_add_func("/search/extraction-wide-chars", test_extraction_wide_chars);
  return g_test_run();
}
