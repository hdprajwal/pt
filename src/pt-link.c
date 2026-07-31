#include "pt-link.h"
#include <string.h>

/* ghostty's scheme-URL branch, transcribed from src/config/url.zig with its
 * scheme list narrowed to the four pt opens. Kept in the same pieces and the
 * same order as the reference so the two can be diffed by eye.
 *
 * The shape is: a scheme, then one or more runs of URL characters each
 * optionally followed by a bracketed word, then a check that the whole thing
 * did not end on punctuation. That last piece is what keeps prose out of the
 * address — "see https://example.com." ends at the "m", and "(https://x)"
 * never takes the closing paren, because ")" is not a URL character unless a
 * "(" opened it inside the match. Both cases come straight out of the
 * corpus in tests/test-link.c. */
#define PT_LINK_SCHEMES  "https?://|mailto:|file:"
#define PT_LINK_IPV6     "(?:\\[[:0-9a-fA-F]+(?:[:0-9a-fA-F]*)+\\](?::[0-9]+)?)"
#define PT_LINK_CHARS    "[\\w\\-.~:/?#@!$&*+,;=%]"
#define PT_LINK_BRACKET  "(?:[\\(\\[]\\w*[\\)\\]])?"
#define PT_LINK_NO_TRAIL "(?<![,.])"

#define PT_LINK_PATTERN                                                       \
  "(?:" PT_LINK_SCHEMES ")"                                                   \
  "(?:" PT_LINK_IPV6 "|" PT_LINK_CHARS "+" PT_LINK_BRACKET ")+"               \
  PT_LINK_NO_TRAIL

/* Compiled once for the process. Every pane hovers against the same pattern,
 * and compiling it per hover would put a regex build on the pointer path. */
static GRegex *link_regex(void) {
  static GRegex *re = NULL;
  static gsize once = 0;
  if (g_once_init_enter(&once)) {
    GError *err = NULL;
    re = g_regex_new(PT_LINK_PATTERN, G_REGEX_OPTIMIZE, 0, &err);
    if (re == NULL) {
      /* A pattern that will not compile is a bug in the literal above, not
       * anything a line of output can cause. Links go dead; nothing else. */
      g_warning("pt: link pattern failed to compile: %s",
                err != NULL ? err->message : "?");
      g_clear_error(&err);
    }
    g_once_init_leave(&once, 1);
  }
  return re;
}

gboolean pt_link_find_at(const char *line, gsize at, PtLinkSpan *out) {
  if (line == NULL || out == NULL) return FALSE;
  gsize len = strlen(line);
  if (len == 0 || at >= len) return FALSE;
  GRegex *re = link_regex();
  if (re == NULL) return FALSE;

  GMatchInfo *mi = NULL;
  gboolean found = FALSE;
  if (g_regex_match_full(re, line, (gssize)len, 0, 0, &mi, NULL)) {
    do {
      gint s = 0, e = 0;
      if (!g_match_info_fetch_pos(mi, 0, &s, &e) || s < 0 || e < 0) break;
      /* Matches arrive left to right and never overlap, so the first one
       * starting past the offset settles it: nothing after it can cover. */
      if ((gsize)s > at) break;
      if (at < (gsize)e) {
        out->start = (gsize)s;
        out->end = (gsize)e;
        found = TRUE;
        break;
      }
    } while (g_match_info_next(mi, NULL));
  }
  g_match_info_free(mi);
  return found;
}
