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
