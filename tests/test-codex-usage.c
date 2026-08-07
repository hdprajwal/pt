#include "pt-codex-usage.h"
#include <glib/gstdio.h>
#include <string.h>

/* Trimmed from a real rollout log, keeping every field the reader touches and
 * the exact types Codex writes them with — used_percent as a float,
 * window_minutes and resets_at as integers, secondary as an explicit null. */
static const char *const REAL_LINE =
  "{\"timestamp\":\"2026-07-31T18:07:05.558Z\",\"type\":\"event_msg\","
  "\"payload\":{\"type\":\"token_count\","
  "\"info\":{\"total_token_usage\":{\"input_tokens\":83480,"
  "\"total_tokens\":84505},\"last_token_usage\":{\"input_tokens\":24680,"
  "\"output_tokens\":607,\"total_tokens\":25287},"
  "\"model_context_window\":258400},"
  "\"rate_limits\":{\"limit_id\":\"codex\",\"limit_name\":null,"
  "\"primary\":{\"used_percent\":33.0,\"window_minutes\":10080,"
  "\"resets_at\":1785938042},\"secondary\":null,"
  "\"credits\":{\"has_credits\":true},\"individual_limit\":null,"
  "\"plan_type\":\"plus\",\"rate_limit_reached_type\":null}}}";

#define NOW 1785521225

static void test_real_line(void) {
  PtUsage u;
  g_assert_true(pt_codex_usage_parse(REAL_LINE, NOW, TRUE, &u));
  g_assert_cmpint(u.kind, ==, PT_AGENT_CODEX);
  g_assert_cmpstr(u.plan, ==, "plus");
  g_assert_false(u.limit_hit);
  /* One window: secondary is null, which is a member that is there and holds
   * nothing — not a second limit reading zero. */
  g_assert_cmpint(u.n_windows, ==, 1);
  g_assert_cmpstr(u.windows[0].label, ==, "weekly");
  g_assert_cmpfloat(u.windows[0].percent, ==, 33.0);
  g_assert_cmpint(u.windows[0].resets_at, ==, 1785938042);
  /* last_token_usage, not total_token_usage: the total is 84505 here and the
   * context only ever held the 25287 the newest request carried. */
  g_assert_cmpint(u.ctx_used, ==, 25287);
  g_assert_cmpint(u.ctx_limit, ==, 258400);
  g_assert_cmpint(pt_usage_context_percent(&u), ==, 9);
  g_assert_cmpstr(u.source, ==, "codex session log");
  /* The parse says what the log holds, never when it was read. */
  g_assert_cmpint(u.fetched_at, ==, 0);
}

/* The limits are account-wide, so another directory's log still reports them;
 * its context fill describes a session that is not on screen, so it does not. */
static void test_limits_without_context(void) {
  PtUsage u;
  g_assert_true(pt_codex_usage_parse(REAL_LINE, NOW, FALSE, &u));
  g_assert_cmpint(u.n_windows, ==, 1);
  g_assert_cmpint(pt_usage_context_percent(&u), ==, -1);
}

/* Every turn writes one, and only the newest describes the account now. */
static void test_takes_the_last(void) {
  char *older = g_strdup(REAL_LINE);
  char *newer = g_strdup(REAL_LINE);
  char *at = strstr(newer, "33.0");
  g_assert_nonnull(at);
  memcpy(at, "81.0", 4);
  char *log = g_strconcat(older, "\n", newer, "\n", NULL);
  PtUsage u;
  g_assert_true(pt_codex_usage_parse(log, NOW, TRUE, &u));
  g_assert_cmpfloat(u.windows[0].percent, ==, 81.0);
  g_free(log);
  g_free(older);
  g_free(newer);
}

/* Only the tail of a log is read, so the first line handed over is routinely
 * half a line. Skipping it must not stop the scan finding the real one. */
static void test_truncated_first_line(void) {
  char *log = g_strconcat("ken\",\"total_tokens\":9}}}\n", REAL_LINE, "\n",
                          NULL);
  PtUsage u;
  g_assert_true(pt_codex_usage_parse(log, NOW, TRUE, &u));
  g_assert_cmpfloat(u.windows[0].percent, ==, 33.0);
  g_free(log);
}

/* Neither a window nor a context reading is nothing to show; taking it would
 * stop the scan short of a line that has something. */
static void test_empty_token_count_is_skipped(void) {
  const char *empty =
    "{\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\"}}";
  char *log = g_strconcat(REAL_LINE, "\n", empty, "\n", NULL);
  PtUsage u;
  g_assert_true(pt_codex_usage_parse(log, NOW, TRUE, &u));
  g_assert_cmpfloat(u.windows[0].percent, ==, 33.0);
  g_free(log);
}

/* A used_percent that is present but no longer a number is no reading at all.
 * Accepting it as 0.0 would put a bar on screen saying the window is
 * untouched, which is the one direction this must not be wrong in. */
static void test_non_numeric_percent(void) {
  PtUsage u;
  const char *stringy =
    "{\"payload\":{\"type\":\"token_count\",\"rate_limits\":{\"primary\":"
    "{\"used_percent\":\"33.0\",\"window_minutes\":10080}}}}";
  g_assert_false(pt_codex_usage_parse(stringy, NOW, TRUE, &u));
  /* With a context reading alongside it the line is still worth taking — but
   * without the window, not with a fabricated one. */
  const char *with_ctx =
    "{\"payload\":{\"type\":\"token_count\","
    "\"info\":{\"last_token_usage\":{\"total_tokens\":100},"
    "\"model_context_window\":1000},"
    "\"rate_limits\":{\"primary\":{\"used_percent\":null,"
    "\"window_minutes\":10080}}}}";
  g_assert_true(pt_codex_usage_parse(with_ctx, NOW, TRUE, &u));
  g_assert_cmpint(u.n_windows, ==, 0);
  g_assert_cmpint(pt_usage_context_percent(&u), ==, 10);
}

static void test_no_token_count(void) {
  PtUsage u;
  g_assert_false(pt_codex_usage_parse("", NOW, TRUE, &u));
  g_assert_false(pt_codex_usage_parse("not json at all\n", NOW, TRUE, &u));
  g_assert_false(pt_codex_usage_parse(
      "{\"type\":\"event_msg\",\"payload\":{\"type\":\"agent_message\"}}",
      NOW, TRUE, &u));
  g_assert_false(pt_codex_usage_parse(NULL, NOW, TRUE, &u));
}

/* Both limits, and the one Codex says was actually hit. A percentage that
 * happens to read 100 is a guess; rate_limit_reached_type is a statement. */
static void test_two_windows_and_limit_hit(void) {
  const char *line =
    "{\"payload\":{\"type\":\"token_count\",\"rate_limits\":{"
    "\"primary\":{\"used_percent\":12,\"window_minutes\":300,"
    "\"resets_at\":1785540000},"
    "\"secondary\":{\"used_percent\":47.5,\"window_minutes\":10080,"
    "\"resets_at\":1785938042},"
    "\"plan_type\":\"pro\",\"rate_limit_reached_type\":\"primary\"}}}";
  PtUsage u;
  g_assert_true(pt_codex_usage_parse(line, NOW, TRUE, &u));
  g_assert_cmpint(u.n_windows, ==, 2);
  g_assert_cmpstr(u.windows[0].label, ==, "5h limit");
  g_assert_cmpfloat(u.windows[0].percent, ==, 12.0);
  g_assert_cmpstr(u.windows[1].label, ==, "weekly");
  g_assert_cmpfloat(u.windows[1].percent, ==, 47.5);
  g_assert_true(u.limit_hit);
  g_assert_cmpstr(u.plan, ==, "pro");
}

/* The rollout log is Codex's own format, not an API contract, and it has
 * already changed shape once. Both spellings of a reset time are read. */
static void test_reset_spellings(void) {
  PtUsage u;
  const char *iso =
    "{\"payload\":{\"type\":\"token_count\",\"rate_limits\":{\"primary\":"
    "{\"used_percent\":5,\"window_minutes\":300,"
    "\"resets_at\":\"2026-07-31T20:00:00Z\"}}}}";
  g_assert_true(pt_codex_usage_parse(iso, NOW, TRUE, &u));
  g_assert_cmpint(u.windows[0].resets_at, ==, 1785528000);

  const char *relative =
    "{\"payload\":{\"type\":\"token_count\",\"rate_limits\":{\"primary\":"
    "{\"used_percent\":5,\"window_minutes\":300,\"resets_in_seconds\":600}}}}";
  g_assert_true(pt_codex_usage_parse(relative, NOW, TRUE, &u));
  g_assert_cmpint(u.windows[0].resets_at, ==, NOW + 600);

  /* Milliseconds, which the two are three orders of magnitude apart on. */
  const char *ms =
    "{\"payload\":{\"type\":\"token_count\",\"rate_limits\":{\"primary\":"
    "{\"used_percent\":5,\"window_minutes\":300,"
    "\"resets_at\":1785938042000}}}}";
  g_assert_true(pt_codex_usage_parse(ms, NOW, TRUE, &u));
  g_assert_cmpint(u.windows[0].resets_at, ==, 1785938042);
}

/* Window names come from the window's own length, so a plan with a shape
 * nobody has shipped yet still labels its bars. */
static void test_window_labels(void) {
  const struct { int minutes; const char *want; } cases[] = {
    { 10080, "weekly" }, { 1440, "daily" }, { 300, "5h limit" },
    { 60, "1h limit" }, { 30, "30m limit" }, { 4320, "3d limit" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    char *line = g_strdup_printf(
        "{\"payload\":{\"type\":\"token_count\",\"rate_limits\":{\"primary\":"
        "{\"used_percent\":1,\"window_minutes\":%d}}}}", cases[i].minutes);
    PtUsage u;
    g_assert_true(pt_codex_usage_parse(line, NOW, TRUE, &u));
    g_assert_cmpstr(u.windows[0].label, ==, cases[i].want);
    g_free(line);
  }
}

static void test_log_cwd(void) {
  const char *header =
    "{\"timestamp\":\"2026-07-31T18:06:34.823Z\",\"type\":\"session_meta\","
    "\"payload\":{\"session_id\":\"019f\",\"cwd\":\"/home/u/dev/proj\","
    "\"cli_version\":\"0.146.0\"}}";
  char *cwd = pt_codex_log_cwd(header);
  g_assert_cmpstr(cwd, ==, "/home/u/dev/proj");
  g_free(cwd);
  g_assert_null(pt_codex_log_cwd("{}"));
  g_assert_null(pt_codex_log_cwd("garbage"));
  g_assert_null(pt_codex_log_cwd(NULL));

  /* Only the header line is searched: a cwd on a later line belongs to some
   * other record, and the caller hands over a chunk, not a line. */
  char *two = g_strconcat("{\"type\":\"session_meta\",\"payload\":{}}\n",
                          header, NULL);
  g_assert_null(pt_codex_log_cwd(two));
  g_free(two);

  /* The value is unescaped by a real parser, not by hand — a path may hold
   * anything a JSON string can. */
  cwd = pt_codex_log_cwd(
      "{\"payload\":{\"cwd\":\"/home/u/a \\\"b\\\" c\\\\d\"}}");
  g_assert_cmpstr(cwd, ==, "/home/u/a \"b\" c\\d");
  g_free(cwd);

  /* Cut off before the value ends: no answer rather than half a path. */
  g_assert_null(pt_codex_log_cwd("{\"payload\":{\"cwd\":\"/home/u/dev"));
}

/* $CODEX_HOME wins when it is set; the read is bounded either way, so a home
 * with no sessions directory at all is a clean miss rather than a warning. */
static void test_home_and_missing_tree(void) {
  g_assert_true(g_setenv("CODEX_HOME", "/nonexistent/codex", TRUE));
  char *home = pt_codex_home();
  g_assert_cmpstr(home, ==, "/nonexistent/codex");
  PtUsage u;
  g_assert_false(pt_codex_usage_read(home, "/tmp", NOW, &u));
  g_free(home);
  g_unsetenv("CODEX_HOME");
  home = pt_codex_home();
  g_assert_true(g_str_has_suffix(home, "/.codex"));
  g_free(home);
}

/* End to end over a real directory tree: the log whose cwd matches the pane
 * wins, and it is the one that contributes the context bar. */
static void test_read_prefers_matching_cwd(void) {
  char *root = g_dir_make_tmp("pt-codex-XXXXXX", NULL);
  g_assert_nonnull(root);
  char *day = g_build_filename(root, "sessions", "2026", "07", "31", NULL);
  g_assert_cmpint(g_mkdir_with_parents(day, 0700), ==, 0);

  char *other_hdr = g_strdup(
      "{\"type\":\"session_meta\",\"payload\":{\"cwd\":\"/somewhere/else\"}}");
  char *other = g_strconcat(other_hdr, "\n", REAL_LINE, "\n", NULL);
  char *p1 = g_build_filename(day, "rollout-a.jsonl", NULL);
  g_assert_true(g_file_set_contents(p1, other, -1, NULL));

  char *mine_hdr =
      g_strdup_printf("{\"type\":\"session_meta\",\"payload\":{\"cwd\":\"%s\"}}",
                      root);
  char *mine_line = g_strdup(REAL_LINE);
  char *at = strstr(mine_line, "33.0");
  memcpy(at, "77.0", 4);
  char *mine = g_strconcat(mine_hdr, "\n", mine_line, "\n", NULL);
  char *p2 = g_build_filename(day, "rollout-b.jsonl", NULL);
  g_assert_true(g_file_set_contents(p2, mine, -1, NULL));

  PtUsage u;
  g_assert_true(pt_codex_usage_read(root, root, NOW, &u));
  g_assert_cmpfloat(u.windows[0].percent, ==, 77.0);
  g_assert_cmpint(u.ctx_used, ==, 25287);

  /* A directory with no log of its own still gets the account-wide limits
   * from whichever log is newest — just without a context bar. */
  g_assert_true(pt_codex_usage_read(root, "/no/such/dir", NOW, &u));
  g_assert_cmpint(u.n_windows, ==, 1);
  g_assert_cmpint(pt_usage_context_percent(&u), ==, -1);

  g_unlink(p1);
  g_unlink(p2);
  /* Innermost out; g_rmdir only succeeds on an empty directory, which is
   * exactly the order this walks them in. */
  for (char *up = g_strdup(day); up != NULL;) {
    g_rmdir(up);
    char *parent = g_str_equal(up, root) ? NULL : g_path_get_dirname(up);
    g_free(up);
    up = parent;
  }
  g_free(p1); g_free(p2); g_free(day);
  g_free(other); g_free(other_hdr);
  g_free(mine); g_free(mine_hdr); g_free(mine_line);
  g_free(root);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/codex/real-line", test_real_line);
  g_test_add_func("/codex/limits-without-context", test_limits_without_context);
  g_test_add_func("/codex/takes-the-last", test_takes_the_last);
  g_test_add_func("/codex/truncated-first-line", test_truncated_first_line);
  g_test_add_func("/codex/empty-token-count", test_empty_token_count_is_skipped);
  g_test_add_func("/codex/non-numeric-percent", test_non_numeric_percent);
  g_test_add_func("/codex/no-token-count", test_no_token_count);
  g_test_add_func("/codex/two-windows", test_two_windows_and_limit_hit);
  g_test_add_func("/codex/reset-spellings", test_reset_spellings);
  g_test_add_func("/codex/window-labels", test_window_labels);
  g_test_add_func("/codex/log-cwd", test_log_cwd);
  g_test_add_func("/codex/home", test_home_and_missing_tree);
  g_test_add_func("/codex/read-prefers-cwd", test_read_prefers_matching_cwd);
  return g_test_run();
}
