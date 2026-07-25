#pragma once
#include <glib.h>

typedef struct {
  gboolean has_fraction;  /* N/M form */
  int done, total;        /* valid when has_fraction */
  gboolean has_percent;   /* N% form */
  int percent;            /* 0..100, valid when has_percent */
} PtProgress;

/* Scan one terminal row for task progress. Recognises, anywhere in the line:
 *  - "N/M" with 0 <= N <= M, M > 0, both plain decimal (e.g. "128/214")
 *  - "N%" with 0 <= N <= 100 (e.g. "74%")
 * Prefers the LAST match in the line (progress counters trail the label).
 * Returns FALSE (out untouched) when nothing is recognised — never fake it. */
gboolean pt_progress_parse_line(const char *line, PtProgress *out);

/* Titles set by the pt prompt snippet carry the last exit status as a
 * prefix: "pt-exit:<code>;<rest of title>". Returns TRUE and strips the
 * prefix (rest points into title) when present. */
gboolean pt_exit_marker_parse(const char *title, int *code, const char **rest);
