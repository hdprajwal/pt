#include "pt-agent-history.h"
#include <glib/gstdio.h>
#include <string.h>

/* Hand-written report with a chosen ts, so ordering tests can pin times
 * instead of racing the clock. */
static void write_report(const char *dir, const char *name,
                         const char *json) {
  char *path = g_build_filename(dir, name, NULL);
  g_assert_true(g_file_set_contents(path, json, -1, NULL));
  g_free(path);
}

static void test_relative_time(void) {
  GDateTime *now = g_date_time_new_from_iso8601("2026-08-21T12:00:00Z", NULL);
  struct { const char *ts; const char *want; } cases[] = {
    { "2026-08-21T11:59:30Z", "now" },
    { "2026-08-21T11:59:00Z", "1m ago" },    /* boundary: 60s is a minute */
    { "2026-08-21T11:58:00Z", "2m ago" },
    { "2026-08-21T11:00:00Z", "1h ago" },
    { "2026-08-21T10:00:00Z", "2h ago" },
    { "2026-08-20T12:00:00Z", "1d ago" },
    { "2026-08-14T12:00:00Z", "7d ago" },
    { "2026-08-21T12:00:30Z", "now" },       /* future stamp: clock skew */
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    GDateTime *ts = g_date_time_new_from_iso8601(cases[i].ts, NULL);
    char *got = pt_agent_history_relative_time(ts, now);
    g_assert_cmpstr(got, ==, cases[i].want);
    g_free(got);
    g_date_time_unref(ts);
  }
  char *empty = pt_agent_history_relative_time(NULL, now);
  g_assert_cmpstr(empty, ==, "");
  g_free(empty);
  g_date_time_unref(now);
}

static void test_load_valid(void) {
  char *dir = g_dir_make_tmp("pt-history-XXXXXX", NULL);
  write_report(dir, "tok.json",
      "{\"version\":1,\"agent\":\"claude\",\"session_id\":\"abc-123\","
      "\"cwd\":\"/home/u/dev/pt\",\"pid\":4242,"
      "\"ts\":\"2026-08-21T10:00:00Z\"}");
  GPtrArray *hist = pt_agent_history_load(dir);
  g_assert_cmpuint(hist->len, ==, 1);
  PtAgentHistoryEntry *e = g_ptr_array_index(hist, 0);
  g_assert_cmpint(e->agent, ==, PT_AGENT_CLAUDE);
  g_assert_cmpstr(e->session_id, ==, "abc-123");
  g_assert_cmpstr(e->cwd, ==, "/home/u/dev/pt");
  g_assert_nonnull(e->ts);
  GDateTime *want = g_date_time_new_from_iso8601("2026-08-21T10:00:00Z", NULL);
  g_assert_cmpint(g_date_time_difference(e->ts, want), ==, 0);
  g_date_time_unref(want);
  g_ptr_array_unref(hist);
  g_rmdir(dir); g_free(dir);
}

static void test_load_skips_bad_files(void) {
  char *dir = g_dir_make_tmp("pt-history-XXXXXX", NULL);
  /* Everything the report gate refuses reads as "not there"; the bad-ts file
   * passes the gate and stays, only without its timestamp. */
  struct { const char *name; const char *text; } bad[] = {
    { "malformed.json", "{ not json" },
    { "unknown-agent.json",
      "{\"version\":1,\"agent\":\"weird\",\"session_id\":\"x\",\"pid\":1}" },
    { "no-session.json",
      "{\"version\":1,\"agent\":\"claude\",\"pid\":1}" },
    { "no-pid.json",
      "{\"version\":1,\"agent\":\"claude\",\"session_id\":\"x\"}" },
    { "not-json-root.json", "[1,2,3]" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++)
    write_report(dir, bad[i].name, bad[i].text);
  /* Passes the gate but its ts is not ISO8601. */
  write_report(dir, "bad-ts.json",
      "{\"version\":1,\"agent\":\"codex\",\"session_id\":\"ok-id\","
      "\"cwd\":\"/tmp/y\",\"pid\":7,\"ts\":\"yesterday-ish\"}");
  /* Not this module's naming: never looked at. */
  write_report(dir, "readme.txt", "hello");

  GPtrArray *hist = pt_agent_history_load(dir);
  g_assert_cmpuint(hist->len, ==, 1);
  PtAgentHistoryEntry *e = g_ptr_array_index(hist, 0);
  g_assert_cmpint(e->agent, ==, PT_AGENT_CODEX);
  g_assert_null(e->ts);   /* unparseable stamp sorts as oldest */
  g_ptr_array_unref(hist);
  g_rmdir(dir); g_free(dir);
}

static void test_load_empty_and_missing_dir(void) {
  char *dir = g_dir_make_tmp("pt-history-XXXXXX", NULL);
  GPtrArray *hist = pt_agent_history_load(dir);
  g_assert_cmpuint(hist->len, ==, 0);
  g_ptr_array_unref(hist);
  hist = pt_agent_history_load("/pt-history/no/such/dir");
  g_assert_nonnull(hist);   /* an absent directory is empty, not a crash */
  g_assert_cmpuint(hist->len, ==, 0);
  g_ptr_array_unref(hist);
  /* Same answer for a NULL dir: an empty list is the empty state everywhere. */
  hist = pt_agent_history_load(NULL);
  g_assert_nonnull(hist);
  g_assert_cmpuint(hist->len, ==, 0);
  g_ptr_array_unref(hist);
  g_rmdir(dir); g_free(dir);
}

static void test_ordering(void) {
  char *dir = g_dir_make_tmp("pt-history-XXXXXX", NULL);
  /* Written deliberately out of order. The two noon entries share a stamp:
   * their relative order comes from the id tie-break, ascending. The one
   * with no ts at all goes last whatever else happens. */
  write_report(dir, "b.json",
      "{\"version\":1,\"agent\":\"claude\",\"session_id\":\"bbb\","
      "\"cwd\":\"/w\",\"pid\":1,\"ts\":\"2026-08-20T09:00:00Z\"}");
  write_report(dir, "n1.json",
      "{\"version\":1,\"agent\":\"codex\",\"session_id\":\"zzz\","
      "\"cwd\":\"/x\",\"pid\":2,\"ts\":\"2026-08-21T12:00:00Z\"}");
  write_report(dir, "n2.json",
      "{\"version\":1,\"agent\":\"claude\",\"session_id\":\"aaa\","
      "\"cwd\":\"/y\",\"pid\":3,\"ts\":\"2026-08-21T12:00:00Z\"}");
  write_report(dir, "old.json",
      "{\"version\":1,\"agent\":\"claude\",\"session_id\":\"ooo\","
      "\"cwd\":\"/z\",\"pid\":4,\"ts\":\"2026-08-01T00:00:00Z\"}");
  write_report(dir, "nostamp.json",
      "{\"version\":1,\"agent\":\"claude\",\"session_id\":\"mmm\","
      "\"cwd\":\"/v\",\"pid\":5}");

  GPtrArray *hist = pt_agent_history_load(dir);
  g_assert_cmpuint(hist->len, ==, 5);
  static const char *const want[] =
      { "aaa", "zzz", "bbb", "ooo", "mmm" };
  for (gsize i = 0; i < G_N_ELEMENTS(want); i++) {
    PtAgentHistoryEntry *e = g_ptr_array_index(hist, i);
    g_assert_cmpstr(e->session_id, ==, want[i]);
  }
  g_ptr_array_unref(hist);
  g_rmdir(dir); g_free(dir);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/agent-history/relative-time", test_relative_time);
  g_test_add_func("/agent-history/load-valid", test_load_valid);
  g_test_add_func("/agent-history/load-skips-bad-files",
                  test_load_skips_bad_files);
  g_test_add_func("/agent-history/empty-and-missing-dir",
                  test_load_empty_and_missing_dir);
  g_test_add_func("/agent-history/ordering", test_ordering);
  return g_test_run();
}
