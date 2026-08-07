#include "pt-claude-usage.h"
#include <glib/gstdio.h>

#define NOW 1785521225

/* The newer shape: one flat array, and the weekly entries say which model
 * they cap. Two weekly bars with the same name would be unreadable, so the
 * model has to reach the label. */
static void test_limits_array(void) {
  const char *body =
    "{\"subscription_type\":\"max\",\"limits\":["
    "{\"type\":\"five_hour\",\"utilization\":12,"
    "\"resets_at\":\"2026-07-31T20:00:00Z\"},"
    "{\"type\":\"seven_day\",\"utilization\":41.5,\"resets_at\":1785938042},"
    "{\"type\":\"seven_day\",\"model\":\"opus\",\"utilization\":6,"
    "\"resets_at\":1785938042}]}";
  PtUsage u;
  g_assert_true(pt_claude_usage_parse(body, NOW, &u));
  g_assert_cmpint(u.kind, ==, PT_AGENT_CLAUDE);
  g_assert_cmpstr(u.plan, ==, "max");
  g_assert_cmpint(u.n_windows, ==, 3);
  g_assert_cmpstr(u.windows[0].label, ==, "5h limit");
  g_assert_cmpfloat(u.windows[0].percent, ==, 12.0);
  g_assert_cmpint(u.windows[0].resets_at, ==, 1785528000);
  g_assert_cmpstr(u.windows[1].label, ==, "weekly");
  g_assert_cmpfloat(u.windows[1].percent, ==, 41.5);
  g_assert_cmpstr(u.windows[2].label, ==, "weekly · opus");
  /* Claude Code records no context window here, so no context bar. */
  g_assert_cmpint(pt_usage_context_percent(&u), ==, -1);
  g_assert_cmpstr(u.source, ==, "anthropic usage api");
}

/* The older shape: a member per window, named for the window, with the
 * per-model cap spelled into the key itself. */
static void test_keyed_windows(void) {
  const char *body =
    "{\"five_hour\":{\"utilization\":8,\"resets_at\":1785528000},"
    "\"seven_day\":{\"utilization\":55,\"resets_at\":1785938042},"
    "\"seven_day_opus\":{\"utilization\":3,\"resets_at\":1785938042}}";
  PtUsage u;
  g_assert_true(pt_claude_usage_parse(body, NOW, &u));
  g_assert_cmpint(u.n_windows, ==, 3);
  g_assert_cmpstr(u.windows[0].label, ==, "5h limit");
  g_assert_cmpstr(u.windows[1].label, ==, "weekly");
  g_assert_cmpstr(u.windows[2].label, ==, "weekly · opus");
  g_assert_cmpfloat(u.windows[2].percent, ==, 3.0);
}

/* A member that is not a window — the plan, a count, anything the endpoint
 * grows next — is stepped over rather than drawn as a bar. */
static void test_keyed_ignores_non_windows(void) {
  const char *body =
    "{\"subscription_type\":\"pro\",\"account_uuid\":\"abc\","
    "\"organization\":{\"name\":\"acme\"},"
    "\"five_hour\":{\"utilization\":8,\"resets_at\":1785528000}}";
  PtUsage u;
  g_assert_true(pt_claude_usage_parse(body, NOW, &u));
  g_assert_cmpint(u.n_windows, ==, 1);
  g_assert_cmpstr(u.plan, ==, "pro");
}

/* The percentage has been spelled more than one way across versions. */
static void test_percent_spellings(void) {
  const char *const bodies[] = {
    "{\"limits\":[{\"type\":\"five_hour\",\"utilization\":20}]}",
    "{\"limits\":[{\"type\":\"five_hour\",\"used_percent\":20}]}",
    "{\"limits\":[{\"type\":\"five_hour\",\"percent_used\":20}]}",
    "{\"limits\":[{\"type\":\"five_hour\",\"usage_percent\":20.0}]}",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(bodies); i++) {
    PtUsage u;
    g_assert_true(pt_claude_usage_parse(bodies[i], NOW, &u));
    g_assert_cmpfloat(u.windows[0].percent, ==, 20.0);
  }
}

/* This is the failure mode the panel must not have. When the format moves
 * again the answer is "the lookup failed", never a bar reading 0% — which
 * would tell the user they have their whole plan left. */
static void test_unrecognised_shape_fails(void) {
  PtUsage u;
  g_assert_false(pt_claude_usage_parse("{}", NOW, &u));
  g_assert_false(pt_claude_usage_parse("{\"limits\":[]}", NOW, &u));
  g_assert_false(pt_claude_usage_parse(
      "{\"windows\":{\"5h\":{\"remaining\":80}}}", NOW, &u));
  g_assert_false(pt_claude_usage_parse("not json", NOW, &u));
  g_assert_false(pt_claude_usage_parse("[1,2,3]", NOW, &u));
  g_assert_false(pt_claude_usage_parse(NULL, NOW, &u));

  /* The subtler form of the same failure: the member is still there, but its
   * type changed. Reading that as 0.0 would draw a bar saying the window is
   * untouched — worse than no bar, because it reads as good news. */
  g_assert_false(pt_claude_usage_parse(
      "{\"limits\":[{\"type\":\"five_hour\",\"utilization\":\"41.5\"}]}",
      NOW, &u));
  g_assert_false(pt_claude_usage_parse(
      "{\"limits\":[{\"type\":\"five_hour\",\"utilization\":true}]}",
      NOW, &u));
  g_assert_false(pt_claude_usage_parse(
      "{\"five_hour\":{\"utilization\":{\"pct\":41.5}}}", NOW, &u));
}

/* An entry with no readable percentage is skipped, not counted as a window at
 * zero. */
static void test_entry_without_percent(void) {
  PtUsage u;
  const char *body =
    "{\"limits\":[{\"type\":\"five_hour\",\"resets_at\":1785528000},"
    "{\"type\":\"seven_day\",\"utilization\":10}]}";
  g_assert_true(pt_claude_usage_parse(body, NOW, &u));
  g_assert_cmpint(u.n_windows, ==, 1);
  g_assert_cmpstr(u.windows[0].label, ==, "weekly");
}

/* More windows than the model holds: the ones that fit are shown, and nothing
 * is written past the array. */
static void test_more_windows_than_fit(void) {
  GString *b = g_string_new("{\"limits\":[");
  for (int i = 0; i < PT_USAGE_MAX_WINDOWS + 4; i++)
    g_string_append_printf(b, "%s{\"type\":\"w%d\",\"utilization\":%d}",
                           i > 0 ? "," : "", i, i);
  g_string_append(b, "]}");
  PtUsage u;
  g_assert_true(pt_claude_usage_parse(b->str, NOW, &u));
  g_assert_cmpint(u.n_windows, ==, PT_USAGE_MAX_WINDOWS);
  g_string_free(b, TRUE);
}

/* What `curl -i` actually hands back. The status decides which of the four
 * failure messages the panel shows, so reading it out of the transcript is
 * not a detail. */
static void test_http_split(void) {
  int status = 0;
  gint64 retry = 0;
  const char *body = pt_claude_http_split(
      "HTTP/2 200 \r\ncontent-type: application/json\r\n\r\n{\"limits\":[]}",
      &status, &retry);
  g_assert_cmpint(status, ==, 200);
  g_assert_cmpint(retry, ==, 0);
  g_assert_cmpstr(body, ==, "{\"limits\":[]}");

  /* The 429 the poll interval and the refresh button both have to respect. */
  body = pt_claude_http_split(
      "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 90\r\n\r\n{}",
      &status, &retry);
  g_assert_cmpint(status, ==, 429);
  g_assert_cmpint(retry, ==, 90);
  g_assert_cmpstr(body, ==, "{}");
  /* Header names are case insensitive on the wire. */
  body = pt_claude_http_split("HTTP/2 429\r\nretry-after:  15\r\n\r\n", &status,
                              &retry);
  g_assert_cmpint(retry, ==, 15);
  /* An HTTP-date is not seconds; reading 0 sends the caller to its own
   * backoff rather than to a guess. */
  body = pt_claude_http_split(
      "HTTP/1.1 429 x\r\nRetry-After: Wed, 21 Oct 2026 07:28:00 GMT\r\n\r\n{}",
      &status, &retry);
  g_assert_cmpint(status, ==, 429);
  g_assert_cmpint(retry, ==, 0);

  /* More than one header block: only the last describes what arrived. */
  body = pt_claude_http_split(
      "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\n\r\n{\"a\":1}",
      &status, &retry);
  g_assert_cmpint(status, ==, 200);
  g_assert_cmpstr(body, ==, "{\"a\":1}");

  /* Bare LF separators, and a response with no body at all. */
  body = pt_claude_http_split("HTTP/1.1 204 No Content\n\n", &status, &retry);
  g_assert_cmpint(status, ==, 204);
  g_assert_cmpstr(body, ==, "");
  body = pt_claude_http_split("HTTP/1.1 500 Oops\r\n", &status, &retry);
  g_assert_cmpint(status, ==, 0);   /* never reached the blank line */
  g_assert_cmpstr(body, ==, "");

  /* curl that produced nothing at all, or something that is not a response. */
  g_assert_cmpstr(pt_claude_http_split("", &status, &retry), ==, "");
  g_assert_cmpint(status, ==, 0);
  g_assert_cmpstr(pt_claude_http_split(NULL, &status, &retry), ==, "");
}

static void write_creds(const char *path, const char *text) {
  g_assert_true(g_file_set_contents(path, text, -1, NULL));
}

static void test_creds(void) {
  char *dir = g_dir_make_tmp("pt-claude-XXXXXX", NULL);
  char *path = g_build_filename(dir, ".credentials.json", NULL);
  PtClaudeCreds c;

  /* expiresAt is milliseconds in this file; the reader normalises it. */
  write_creds(path,
      "{\"claudeAiOauth\":{\"accessToken\":\"sk-ant-oat01-xyz\","
      "\"refreshToken\":\"sk-ant-ort01-abc\","
      "\"expiresAt\":1785938042000,\"subscriptionType\":\"max\"}}");
  g_assert_true(pt_claude_creds_read(path, &c));
  g_assert_cmpstr(c.token, ==, "sk-ant-oat01-xyz");
  g_assert_cmpint(c.expires_at, ==, 1785938042);
  g_assert_cmpstr(c.plan, ==, "max");
  pt_claude_creds_clear(&c);
  g_assert_null(c.token);

  /* A file with no token is the same answer as no file: not logged in. */
  write_creds(path, "{\"claudeAiOauth\":{\"expiresAt\":1785938042000}}");
  g_assert_false(pt_claude_creds_read(path, &c));
  pt_claude_creds_clear(&c);

  write_creds(path, "garbage");
  g_assert_false(pt_claude_creds_read(path, &c));
  pt_claude_creds_clear(&c);

  g_unlink(path);
  g_assert_false(pt_claude_creds_read(path, &c));
  pt_claude_creds_clear(&c);

  g_rmdir(dir);
  g_free(path);
  g_free(dir);

  char *def = pt_claude_creds_path();
  g_assert_true(g_str_has_suffix(def, "/.claude/.credentials.json"));
  g_free(def);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/claude/limits-array", test_limits_array);
  g_test_add_func("/claude/keyed-windows", test_keyed_windows);
  g_test_add_func("/claude/keyed-ignores-non-windows",
                  test_keyed_ignores_non_windows);
  g_test_add_func("/claude/percent-spellings", test_percent_spellings);
  g_test_add_func("/claude/unrecognised-shape", test_unrecognised_shape_fails);
  g_test_add_func("/claude/entry-without-percent", test_entry_without_percent);
  g_test_add_func("/claude/more-windows-than-fit", test_more_windows_than_fit);
  g_test_add_func("/claude/http-split", test_http_split);
  g_test_add_func("/claude/creds", test_creds);
  return g_test_run();
}
