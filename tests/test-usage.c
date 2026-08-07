#include "pt-usage.h"
#include <string.h>

static void expect_duration(gint64 seconds, const char *want) {
  char *got = pt_usage_format_duration(seconds);
  g_assert_cmpstr(got, ==, want);
  g_free(got);
}

/* The countdown under a bar. Two units at most: on a 266px rail "2d 4h 13m"
 * would push the percentage off the row, and nobody plans around the minutes
 * of a weekly limit. */
static void test_duration(void) {
  expect_duration(0, "now");
  expect_duration(-500, "now");     /* the window already turned over */
  expect_duration(30, "<1m");
  expect_duration(59, "<1m");
  expect_duration(60, "1m");
  expect_duration(45 * 60, "45m");
  expect_duration(3600, "1h");      /* an exact hour drops the empty minutes */
  expect_duration(3 * 3600 + 20 * 60, "3h 20m");
  expect_duration(23 * 3600 + 59 * 60, "23h 59m");
  expect_duration(24 * 3600, "1d");
  expect_duration(2 * 86400 + 4 * 3600, "2d 4h");
  expect_duration(7 * 86400, "7d");
}

static void expect_age(gint64 fetched, gint64 now, const char *want) {
  char *got = pt_usage_format_age(fetched, now);
  g_assert_cmpstr(got, ==, want);
  g_free(got);
}

static void test_age(void) {
  g_assert_null(pt_usage_format_age(0, 1000));   /* never fetched */
  expect_age(1000, 1000, "just now");
  expect_age(1000, 1040, "just now");
  /* The whole minute below the first "1m ago" is "just now", not "0m ago" —
   * a minute is the resolution everything here works in. */
  expect_age(1000, 1059, "just now");
  expect_age(1000, 1060, "1m ago");
  expect_age(1000, 1120, "2m ago");
  expect_age(1000, 1000 + 7200, "2h ago");
  expect_age(1000, 1000 + 3 * 86400, "3d ago");
  /* A clock that stepped backwards must not print a negative age: the reading
   * is current, whatever the clock now says. */
  expect_age(2000, 1000, "just now");
}

/* A percentage a provider reported out of range still says the window is
 * full; the bar just must not be drawn past its track. */
static void test_window_clamp(void) {
  PtUsage u;
  pt_usage_clear(&u);
  pt_usage_add_window(&u, "5h limit", 103.5, 0);
  pt_usage_add_window(&u, "weekly", -2.0, 0);
  g_assert_cmpint(u.n_windows, ==, 2);
  g_assert_cmpfloat(u.windows[0].percent, ==, 100.0);
  g_assert_cmpfloat(u.windows[1].percent, ==, 0.0);
  g_assert_cmpstr(u.windows[0].label, ==, "5h limit");
}

/* A provider that grows a window past what the model holds shows the ones
 * that fit, and does not scribble past the array. */
static void test_window_overflow(void) {
  PtUsage u;
  pt_usage_clear(&u);
  for (int i = 0; i < PT_USAGE_MAX_WINDOWS + 3; i++)
    pt_usage_add_window(&u, "w", 10.0, 0);
  g_assert_cmpint(u.n_windows, ==, PT_USAGE_MAX_WINDOWS);
}

/* A label too long for the field is cut on a character, never through one.
 * These come from window names pt did not choose and carry a "·" it added, so
 * a byte-wise cut would put invalid UTF-8 into a GtkLabel. */
static void test_label_truncation(void) {
  PtUsage u;
  pt_usage_clear(&u);
  gsize cap = sizeof(u.windows[0].label);
  /* Land the multi-byte "·" exactly on the boundary: the label is cut before
   * it, not through it. */
  for (gsize pad = 0; pad < 4; pad++) {
    pt_usage_clear(&u);
    char *name = g_strnfill(cap - 3 + pad, 'x');
    char *label = g_strdup_printf("%s · opus", name);
    pt_usage_add_window(&u, label, 10.0, 0);
    g_assert_true(g_utf8_validate(u.windows[0].label, -1, NULL));
    g_assert_cmpuint(strlen(u.windows[0].label), <, cap);
    g_free(label);
    g_free(name);
  }
  /* A label that fits is not touched. */
  pt_usage_clear(&u);
  pt_usage_add_window(&u, "weekly · opus", 10.0, 0);
  g_assert_cmpstr(u.windows[0].label, ==, "weekly · opus");
  /* And no label at all is the empty one, not a crash. */
  pt_usage_clear(&u);
  pt_usage_add_window(&u, NULL, 10.0, 0);
  g_assert_cmpstr(u.windows[0].label, ==, "");
}

/* A reset time of 0 is "the source did not say", which is not the same as a
 * window that resets at the epoch. */
static void test_no_reset_time(void) {
  PtUsage u;
  pt_usage_clear(&u);
  pt_usage_add_window(&u, "weekly", 10.0, -5);
  g_assert_cmpint(u.windows[0].resets_at, ==, 0);
}

static void test_context(void) {
  PtUsage u;
  pt_usage_clear(&u);
  g_assert_cmpint(pt_usage_context_percent(&u), ==, -1);  /* none recorded */
  u.ctx_limit = 200000;
  g_assert_cmpint(pt_usage_context_percent(&u), ==, -1);  /* limit but no use */
  u.ctx_used = 50000;
  g_assert_cmpint(pt_usage_context_percent(&u), ==, 25);
  /* A request that carried more than the window says it holds is still a full
   * bar, not a bar past its end. */
  u.ctx_used = 400000;
  g_assert_cmpint(pt_usage_context_percent(&u), ==, 100);
}

static void test_has_data(void) {
  PtUsage u;
  pt_usage_clear(&u);
  g_assert_false(pt_usage_has_data(&u));
  g_assert_false(pt_usage_has_data(NULL));
  u.fetched_at = 1;
  g_assert_true(pt_usage_has_data(&u));
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/usage/duration", test_duration);
  g_test_add_func("/usage/age", test_age);
  g_test_add_func("/usage/window-clamp", test_window_clamp);
  g_test_add_func("/usage/window-overflow", test_window_overflow);
  g_test_add_func("/usage/label-truncation", test_label_truncation);
  g_test_add_func("/usage/no-reset-time", test_no_reset_time);
  g_test_add_func("/usage/context", test_context);
  g_test_add_func("/usage/has-data", test_has_data);
  return g_test_run();
}
