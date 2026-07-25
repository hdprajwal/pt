#include <glib.h>
#include "pt-status-parse.h"

int main(void) {
  PtProgress p;
  g_assert_true(pt_progress_parse_line("cargo test  128/214", &p));
  g_assert_true(p.has_fraction); g_assert_cmpint(p.done, ==, 128);
  g_assert_cmpint(p.total, ==, 214);
  g_assert_true(pt_progress_parse_line("vite build  74%", &p));
  g_assert_true(p.has_percent); g_assert_cmpint(p.percent, ==, 74);
  g_assert_true(pt_progress_parse_line("step 3/8: linking 50%", &p));
  g_assert_true(p.has_percent); g_assert_cmpint(p.percent, ==, 50); /* last match wins */
  g_assert_false(pt_progress_parse_line("plain shell output", &p));
  g_assert_false(pt_progress_parse_line("date 2026/07", &p));   /* 2026/07: N>M → reject */
  g_assert_false(pt_progress_parse_line("owner/repo", &p));     /* no digits → reject */
  g_assert_false(pt_progress_parse_line("", &p));
  g_assert_false(pt_progress_parse_line("120%", &p));           /* >100 → reject */

  int code; const char *rest;
  g_assert_true(pt_exit_marker_parse("pt-exit:0;~/dev", &code, &rest));
  g_assert_cmpint(code, ==, 0); g_assert_cmpstr(rest, ==, "~/dev");
  g_assert_true(pt_exit_marker_parse("pt-exit:127;", &code, &rest));
  g_assert_cmpint(code, ==, 127); g_assert_cmpstr(rest, ==, "");
  g_assert_false(pt_exit_marker_parse("~/dev/personal", &code, &rest));
  g_assert_false(pt_exit_marker_parse("pt-exit:x;bad", &code, &rest));

  /* --- additional edge cases --- */
  g_assert_false(pt_progress_parse_line(NULL, &p));
  g_assert_false(pt_progress_parse_line("0/0", &p));            /* M == 0 */
  g_assert_false(pt_progress_parse_line("v1.2.3 released", &p)); /* version */
  g_assert_false(pt_progress_parse_line("built 2026/07/25", &p)); /* date */
  g_assert_false(pt_progress_parse_line("scale 1/2.5", &p));     /* ratio */
  g_assert_false(pt_progress_parse_line("sha1/2", &p));          /* identifier */
  /* absurdly long digit runs are ids, not counters (and would overflow) */
  g_assert_false(pt_progress_parse_line(
      "99999999999999999999/99999999999999999999", &p));

  g_assert_true(pt_progress_parse_line("0/5 done", &p));
  g_assert_true(p.has_fraction); g_assert_cmpint(p.done, ==, 0);
  g_assert_cmpint(p.total, ==, 5); g_assert_false(p.has_percent);
  g_assert_true(pt_progress_parse_line("100%", &p));
  g_assert_true(p.has_percent); g_assert_cmpint(p.percent, ==, 100);
  g_assert_false(p.has_fraction);
  g_assert_true(pt_progress_parse_line("74% then 9/10", &p));   /* last wins */
  g_assert_true(p.has_fraction); g_assert_cmpint(p.done, ==, 9);
  g_assert_cmpint(p.total, ==, 10); g_assert_false(p.has_percent);

  /* a failed parse must leave *out untouched */
  PtProgress keep = { .has_fraction = TRUE, .done = 7, .total = 9,
                      .has_percent = TRUE, .percent = 42 };
  p = keep;
  g_assert_false(pt_progress_parse_line("nothing here", &p));
  g_assert_cmpint(p.done, ==, 7); g_assert_cmpint(p.total, ==, 9);
  g_assert_cmpint(p.percent, ==, 42);

  g_assert_false(pt_exit_marker_parse(NULL, &code, &rest));
  g_assert_false(pt_exit_marker_parse("pt-exit:", &code, &rest));
  g_assert_false(pt_exit_marker_parse("pt-exit:;x", &code, &rest));
  g_assert_false(pt_exit_marker_parse("pt-exit:12", &code, &rest));   /* no ';' */
  g_assert_false(pt_exit_marker_parse("pt-exit:256;x", &code, &rest)); /* > 255 */
  g_assert_false(pt_exit_marker_parse("pt-exit:-1;x", &code, &rest));
  g_assert_false(pt_exit_marker_parse(" pt-exit:0;x", &code, &rest));
  g_assert_true(pt_exit_marker_parse("pt-exit:255;pt-exit:1;x", &code, &rest));
  g_assert_cmpint(code, ==, 255); g_assert_cmpstr(rest, ==, "pt-exit:1;x");
  return 0;
}
