#include <glib.h>
#include "pt-fuzzy.h"

/* ---------- pt_fuzzy_score ---------- */
static void test_score_basics(void) {
  g_assert_cmpint(pt_fuzzy_score("", "anything"), ==, 1);
  g_assert_cmpint(pt_fuzzy_score("abc", "zzz"), ==, 0);
  g_assert_true(pt_fuzzy_score("ptw", "pt-window") > 0);
  g_assert_true(pt_fuzzy_score("PTW", "pt-window") > 0);   /* case-insensitive */
  g_assert_cmpint(pt_fuzzy_score("xyz", "pt-window"), ==, 0);
  /* prefix beats scattered */
  g_assert_true(pt_fuzzy_score("side", "sidebar") >
                pt_fuzzy_score("side", "s-i-d-e-bar"));
  /* word-boundary beats mid-word */
  g_assert_true(pt_fuzzy_score("win", "pt-window") >
                pt_fuzzy_score("win", "darwinia"));
}

static void test_score_edges(void) {
  g_assert_cmpint(pt_fuzzy_score(NULL, "pt-window"), ==, 0);
  g_assert_cmpint(pt_fuzzy_score("pt", NULL), ==, 0);
  g_assert_cmpint(pt_fuzzy_score("", ""), ==, 1);
  g_assert_cmpint(pt_fuzzy_score("pt", ""), ==, 0);
  /* out-of-order needle never matches */
  g_assert_cmpint(pt_fuzzy_score("wp", "pt-window"), ==, 0);
  /* every char must be consumed, not just some */
  g_assert_cmpint(pt_fuzzy_score("ptx", "pt-window"), ==, 0);
  /* exact match scores at least as well as a longer haystack */
  g_assert_true(pt_fuzzy_score("pt", "pt") >= pt_fuzzy_score("pt", "apt"));
  /* mixed case on both sides */
  g_assert_cmpint(pt_fuzzy_score("PtW", "PT-Window"),
                  ==, pt_fuzzy_score("ptw", "pt-window"));
  /* non-ASCII is compared bytewise: still matches itself */
  g_assert_true(pt_fuzzy_score("é", "café") > 0);
  g_assert_true(pt_fuzzy_score("café", "café-app") > 0);
}

static void test_score_separators(void) {
  /* every separator gets the word-boundary bonus */
  g_assert_true(pt_fuzzy_score("w", "a/window") > pt_fuzzy_score("w", "aawindow"));
  g_assert_true(pt_fuzzy_score("w", "a_window") > pt_fuzzy_score("w", "aawindow"));
  g_assert_true(pt_fuzzy_score("w", "a.window") > pt_fuzzy_score("w", "aawindow"));
  g_assert_true(pt_fuzzy_score("w", "a window") > pt_fuzzy_score("w", "aawindow"));
}

/* ---------- pt_fuzzy_rank ---------- */
/* Comma-joined indices, so a whole ranking is one readable assertion. */
static char *ranked(const char *const *labels, int n, const char *query,
                    int max_out) {
  int idx[16];
  g_assert_cmpint(max_out, <=, (int)G_N_ELEMENTS(idx));
  int kept = pt_fuzzy_rank(labels, n, query, idx, max_out);
  g_assert_cmpint(kept, <=, max_out);
  GString *s = g_string_new(NULL);
  for (int i = 0; i < kept; i++)
    g_string_append_printf(s, i > 0 ? ",%d" : "%d", idx[i]);
  return g_string_free(s, FALSE);
}

/* An empty query scores everything the same, so the answer is the first
 * max_out items in their given order — what the palette shows when it opens. */
static void test_rank_empty_query(void) {
  const char *const labels[] = { "a", "b", "c", "d", "e", "f", "g" };
  char *r = ranked(labels, 7, "", 6);
  g_assert_cmpstr(r, ==, "0,1,2,3,4,5");
  g_free(r);
  r = ranked(labels, 7, "", 3);
  g_assert_cmpstr(r, ==, "0,1,2");
  g_free(r);
}

/* Equal scores keep input order: the selection is stable, so re-ranking an
 * unchanged list cannot reshuffle the rows under the user's cursor. */
static void test_rank_stability(void) {
  const char *const same[] = { "pt-a", "pt-b", "pt-c", "pt-d", "pt-e",
                              "pt-f", "pt-g", "pt-h" };
  char *r = ranked(same, 8, "pt", 6);
  g_assert_cmpstr(r, ==, "0,1,2,3,4,5");
  g_free(r);
  /* Ties among the leaders, too: every one of these scores the same, so the
   * kept ones are the earliest — a later tie never displaces the worst kept. */
  r = ranked(same, 8, "pt-", 2);
  g_assert_cmpstr(r, ==, "0,1");
  g_free(r);
}

/* Score order wins over input order: a better late item climbs, and it is the
 * one that survives a max_out too small to hold everything. */
static void test_rank_by_score(void) {
  const char *const labels[] = { "s-i-d-e-bar", "sidebar", "aside" };
  int idx[3];
  int kept = pt_fuzzy_rank(labels, 3, "side", idx, 3);
  g_assert_cmpint(kept, ==, 3);
  g_assert_cmpint(idx[0], ==, 1);   /* the one real prefix match */
  /* Whatever the scorer's exact numbers, the output is score-descending. */
  for (int i = 1; i < kept; i++)
    g_assert_true(pt_fuzzy_score("side", labels[idx[i - 1]]) >=
                  pt_fuzzy_score("side", labels[idx[i]]));
  kept = pt_fuzzy_rank(labels, 3, "side", idx, 1);
  g_assert_cmpint(kept, ==, 1);
  g_assert_cmpint(idx[0], ==, 1);
}

/* Non-matches are dropped; fewer matches than max_out is normal. */
static void test_rank_filters(void) {
  const char *const labels[] = { "window", "zzz", "widget", NULL, "wig" };
  char *r = ranked(labels, 5, "wig", 6);
  g_assert_cmpstr(r, ==, "4,2");   /* "wig" beats "widget"; NULL scores 0 */
  g_free(r);
  r = ranked(labels, 5, "qqq", 6);
  g_assert_cmpstr(r, ==, "");
  g_free(r);
}

static void test_rank_degenerate(void) {
  const char *const labels[] = { "a", "b" };
  int idx[4] = { -7, -7, -7, -7 };
  g_assert_cmpint(pt_fuzzy_rank(labels, 2, "a", idx, 0), ==, 0);
  g_assert_cmpint(pt_fuzzy_rank(labels, 0, "a", idx, 4), ==, 0);
  g_assert_cmpint(pt_fuzzy_rank(NULL, 2, "a", idx, 4), ==, 0);
  g_assert_cmpint(pt_fuzzy_rank(labels, 2, "a", NULL, 4), ==, 0);
  g_assert_cmpint(idx[0], ==, -7);   /* nothing written on any of those */
  /* A NULL query matches nothing: it is not the same as an empty one. */
  g_assert_cmpint(pt_fuzzy_rank(labels, 2, NULL, idx, 4), ==, 0);
}

/* ---------- pt_fuzzy_rank_scored ----------
 * The primitive the palette calls directly, because its score per item is the
 * better of two strings (an item's name and its detail). */
static void test_rank_scored(void) {
  const int scores[] = { 3, 9, 3, 0, 9, 4 };
  int idx[3];
  int kept = pt_fuzzy_rank_scored(scores, 6, idx, 3);
  g_assert_cmpint(kept, ==, 3);
  g_assert_cmpint(idx[0], ==, 1);   /* 9, first of its ties */
  g_assert_cmpint(idx[1], ==, 4);   /* 9, the later tie */
  g_assert_cmpint(idx[2], ==, 5);   /* 4 */
  /* Zero and negative scores never make the list. */
  const int none[] = { 0, -1, 0 };
  g_assert_cmpint(pt_fuzzy_rank_scored(none, 3, idx, 3), ==, 0);
  g_assert_cmpint(pt_fuzzy_rank_scored(NULL, 3, idx, 3), ==, 0);
  g_assert_cmpint(pt_fuzzy_rank_scored(scores, 6, idx, 0), ==, 0);
  /* Fewer scoring items than asked for. */
  const int one[] = { 0, 5, 0 };
  g_assert_cmpint(pt_fuzzy_rank_scored(one, 3, idx, 3), ==, 1);
  g_assert_cmpint(idx[0], ==, 1);
}

/* pt_fuzzy_rank is that primitive over pt_fuzzy_score, and the two agree. */
static void test_rank_agrees_with_scored(void) {
  const char *const labels[] = { "pt-window", "window", "wind", "zzz" };
  int scores[4];
  for (int i = 0; i < 4; i++) scores[i] = pt_fuzzy_score("win", labels[i]);
  int a[4], b[4];
  int ka = pt_fuzzy_rank(labels, 4, "win", a, 4);
  int kb = pt_fuzzy_rank_scored(scores, 4, b, 4);
  g_assert_cmpint(ka, ==, kb);
  for (int i = 0; i < ka; i++) g_assert_cmpint(a[i], ==, b[i]);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/fuzzy/score/basics", test_score_basics);
  g_test_add_func("/fuzzy/score/edges", test_score_edges);
  g_test_add_func("/fuzzy/score/separators", test_score_separators);
  g_test_add_func("/fuzzy/rank/empty-query", test_rank_empty_query);
  g_test_add_func("/fuzzy/rank/stability", test_rank_stability);
  g_test_add_func("/fuzzy/rank/by-score", test_rank_by_score);
  g_test_add_func("/fuzzy/rank/filters", test_rank_filters);
  g_test_add_func("/fuzzy/rank/degenerate", test_rank_degenerate);
  g_test_add_func("/fuzzy/rank/scored", test_rank_scored);
  g_test_add_func("/fuzzy/rank/scored-agrees", test_rank_agrees_with_scored);
  return g_test_run();
}
