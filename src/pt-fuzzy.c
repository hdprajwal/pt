#include "pt-fuzzy.h"

static gboolean is_sep(char c) {
  return c == '/' || c == '-' || c == '_' || c == '.' || c == ' ';
}

int pt_fuzzy_score(const char *needle, const char *haystack) {
  if (needle == NULL || haystack == NULL) return 0;
  if (needle[0] == '\0') return 1;
  int score = 0;
  gboolean prev_matched = FALSE;
  const char *n = needle;
  for (const char *h = haystack; *h != '\0' && *n != '\0'; h++) {
    if (g_ascii_tolower(*h) == g_ascii_tolower(*n)) {
      score += 1;
      /* A consecutive run must outweigh a word boundary, otherwise
       * "s-i-d-e-bar" (four boundaries) would beat "sidebar" (one
       * boundary + three consecutive) for the needle "side". */
      if (prev_matched) score += 4;                     /* consecutive run */
      if (h == haystack || is_sep(h[-1])) score += 3;   /* word boundary */
      prev_matched = TRUE;
      n++;
    } else {
      prev_matched = FALSE;
    }
  }
  return *n == '\0' ? score + 1 : 0;   /* +1 so a match is never 0 */
}

int pt_fuzzy_rank_scored(const int *scores, int n, int *out_idx, int max_out) {
  if (scores == NULL || out_idx == NULL || max_out <= 0) return 0;
  int kept = 0;
  for (int i = 0; i < n; i++) {
    int s = scores[i];
    if (s <= 0) continue;
    /* Full and no better than the worst kept: a tie loses to the earlier item,
     * which is what keeps the selection stable. */
    if (kept == max_out && s <= scores[out_idx[max_out - 1]]) continue;
    /* Insert, shifting the tail down; the item at the last slot falls off.
     * A kept index doubles as its own score's address, so the shift needs no
     * second array to walk. */
    int pos = kept < max_out ? kept : max_out - 1;
    while (pos > 0 && scores[out_idx[pos - 1]] < s) {
      out_idx[pos] = out_idx[pos - 1];
      pos--;
    }
    out_idx[pos] = i;
    if (kept < max_out) kept++;
  }
  return kept;
}

int pt_fuzzy_rank(const char *const *labels, int n, const char *query,
                  int *out_idx, int max_out) {
  if (labels == NULL || n <= 0 || out_idx == NULL || max_out <= 0) return 0;
  int *scores = g_new(int, (gsize)n);
  for (int i = 0; i < n; i++) scores[i] = pt_fuzzy_score(query, labels[i]);
  int kept = pt_fuzzy_rank_scored(scores, n, out_idx, max_out);
  g_free(scores);
  return kept;
}
