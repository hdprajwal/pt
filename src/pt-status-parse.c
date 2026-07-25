#include "pt-status-parse.h"
#include <stdlib.h>
#include <string.h>

/* Largest counter value we treat as a plausible progress number. Anything
 * longer is a hash, an id or a timestamp — not a task counter — and would
 * also risk overflowing the int fields. */
#define PT_PROGRESS_MAX 1000000000L

/* Parse a plain decimal run at s (no sign, no whitespace skipping).
 * Returns FALSE when s does not start with a digit or the value is out of
 * range; *end always advances past the digit run so the caller can resume. */
static gboolean parse_uint(const char *s, long *value, const char **end) {
  const char *p = s;
  long v = 0;
  gboolean overflow = FALSE;
  while (g_ascii_isdigit(*p)) {
    if (v > PT_PROGRESS_MAX) overflow = TRUE;
    else v = v * 10 + (*p - '0');
    p++;
  }
  *end = p;
  if (p == s || overflow || v > PT_PROGRESS_MAX) return FALSE;
  *value = v;
  return TRUE;
}

gboolean pt_progress_parse_line(const char *line, PtProgress *out) {
  if (line == NULL || out == NULL) return FALSE;

  PtProgress best = {0};
  gboolean found = FALSE;

  for (const char *s = line; *s != '\0'; s++) {
    if (!g_ascii_isdigit(*s)) continue;

    /* The digits must start a number: a preceding letter, digit or dot means
     * we are inside an identifier or a version string (sha1, 1.5, v2). */
    if (s > line && (g_ascii_isalnum(s[-1]) || s[-1] == '.')) {
      while (g_ascii_isdigit(*s)) s++;
      if (*s == '\0') break;
      continue;
    }

    long a = 0;
    const char *end = NULL;
    gboolean a_ok = parse_uint(s, &a, &end);

    if (a_ok && *end == '%' && a <= 100) {
      best = (PtProgress){ .has_percent = TRUE, .percent = (int)a };
      found = TRUE;
      s = end; /* loop's s++ moves past the '%' */
      continue;
    }

    if (*end == '/' && g_ascii_isdigit(end[1])) {
      long b = 0;
      const char *end2 = NULL;
      gboolean b_ok = parse_uint(end + 1, &b, &end2);
      /* Reject trailing junk glued to the denominator (dates, versions,
       * paths): "2026/07/25", "1/2.3", "3/4x" are not progress counters. */
      if (a_ok && b_ok && b > 0 && a <= b &&
          !g_ascii_isalnum(*end2) && *end2 != '/' && *end2 != '.') {
        best = (PtProgress){ .has_fraction = TRUE, .done = (int)a, .total = (int)b };
        found = TRUE;
      }
      s = end2 - 1; /* loop's s++ resumes at end2 */
      continue;
    }

    s = end - 1; /* loop's s++ resumes at end */
  }

  if (found) *out = best;
  return found;
}

gboolean pt_exit_marker_parse(const char *title, int *code, const char **rest) {
  static const char prefix[] = "pt-exit:";
  if (title == NULL || code == NULL || rest == NULL) return FALSE;
  if (strncmp(title, prefix, sizeof(prefix) - 1) != 0) return FALSE;

  const char *p = title + sizeof(prefix) - 1;
  if (!g_ascii_isdigit(*p)) return FALSE;
  char *end = NULL;
  long v = strtol(p, &end, 10);
  if (end == p || *end != ';' || v < 0 || v > 255) return FALSE;

  *code = (int)v;
  *rest = end + 1;
  return TRUE;
}
