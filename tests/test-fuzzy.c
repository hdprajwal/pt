#include <glib.h>
#include "pt-fuzzy.h"

int main(void) {
  g_assert_cmpint(pt_fuzzy_score("", "anything"), ==, 1);
  g_assert_cmpint(pt_fuzzy_score("abc", "zzz"), ==, 0);
  g_assert_true(pt_fuzzy_score("ptw", "pt-window") > 0);
  g_assert_true(pt_fuzzy_score("PTW", "pt-window") > 0);          /* case-insensitive */
  g_assert_cmpint(pt_fuzzy_score("xyz", "pt-window"), ==, 0);
  /* prefix beats scattered */
  g_assert_true(pt_fuzzy_score("side", "sidebar") >
                pt_fuzzy_score("side", "s-i-d-e-bar"));
  /* word-boundary beats mid-word */
  g_assert_true(pt_fuzzy_score("win", "pt-window") >
                pt_fuzzy_score("win", "darwinia"));

  /* --- additional cases (never weaken the above) --- */
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
  /* every separator gets the word-boundary bonus */
  g_assert_true(pt_fuzzy_score("w", "a/window") > pt_fuzzy_score("w", "aawindow"));
  g_assert_true(pt_fuzzy_score("w", "a_window") > pt_fuzzy_score("w", "aawindow"));
  g_assert_true(pt_fuzzy_score("w", "a.window") > pt_fuzzy_score("w", "aawindow"));
  g_assert_true(pt_fuzzy_score("w", "a window") > pt_fuzzy_score("w", "aawindow"));
  return 0;
}
