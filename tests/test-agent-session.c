#include "pt-agent-session.h"
#include <gio/gio.h>          /* GSubprocess, for driving the real helper */
#include <glib/gstdio.h>
#include <string.h>

static void test_kind_names(void) {
  g_assert_cmpstr(pt_agent_session_kind_name(PT_AGENT_CLAUDE), ==, "claude");
  g_assert_cmpstr(pt_agent_session_kind_name(PT_AGENT_CODEX), ==, "codex");
  g_assert_null(pt_agent_session_kind_name(PT_AGENT_NONE));
  g_assert_cmpint(pt_agent_session_kind_from_name("claude"), ==, PT_AGENT_CLAUDE);
  g_assert_cmpint(pt_agent_session_kind_from_name("codex"), ==, PT_AGENT_CODEX);
  g_assert_cmpint(pt_agent_session_kind_from_name("weird"), ==, PT_AGENT_NONE);
  g_assert_cmpint(pt_agent_session_kind_from_name(NULL), ==, PT_AGENT_NONE);
}

static void test_token(void) {
  char *a = pt_agent_session_token_new();
  char *b = pt_agent_session_token_new();
  g_assert_cmpuint(strlen(a), ==, 16);
  for (const char *p = a; *p != '\0'; p++)
    g_assert_true(g_ascii_isxdigit(*p) && !g_ascii_isupper(*p));
  g_assert_cmpstr(a, !=, b);
  g_free(a); g_free(b);
}

static void test_report_roundtrip(void) {
  char *dir = g_dir_make_tmp("pt-agent-XXXXXX", NULL);
  char *path = g_build_filename(dir, "tok.json", NULL);
  GError *err = NULL;
  g_assert_true(pt_agent_session_report_write(path, PT_AGENT_CLAUDE,
                                              "abc-123", "/tmp/x", 4242, &err));
  g_assert_no_error(err);
  PtAgentSessionReport *r = pt_agent_session_report_load(path);
  g_assert_nonnull(r);
  g_assert_cmpint(r->agent, ==, PT_AGENT_CLAUDE);
  g_assert_cmpstr(r->session_id, ==, "abc-123");
  g_assert_cmpstr(r->cwd, ==, "/tmp/x");
  g_assert_cmpint(r->pid, ==, 4242);
  pt_agent_session_report_free(r);
  g_remove(path); g_rmdir(dir); g_free(path); g_free(dir);
}

static void test_report_load_rejects(void) {
  char *dir = g_dir_make_tmp("pt-agent-XXXXXX", NULL);
  struct { const char *name; const char *text; } bad[] = {
    { "malformed.json", "{ not json" },
    { "unknown-agent.json",
      "{\"version\":1,\"agent\":\"weird\",\"session_id\":\"x\",\"pid\":1}" },
    { "empty-id.json",
      "{\"version\":1,\"agent\":\"claude\",\"session_id\":\"\",\"pid\":1}" },
    { "future.json",
      "{\"version\":99,\"agent\":\"claude\",\"session_id\":\"x\",\"pid\":1}" },
    { "no-pid.json",
      "{\"version\":1,\"agent\":\"claude\",\"session_id\":\"x\"}" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    char *p = g_build_filename(dir, bad[i].name, NULL);
    g_assert_true(g_file_set_contents(p, bad[i].text, -1, NULL));
    g_assert_null(pt_agent_session_report_load(p));
    g_remove(p); g_free(p);
  }
  char *missing = g_build_filename(dir, "absent.json", NULL);
  g_assert_null(pt_agent_session_report_load(missing));
  g_free(missing); g_rmdir(dir); g_free(dir);
}

static void test_report_matches(void) {
  PtAgentSessionReport r = { PT_AGENT_CLAUDE, "abc", "/x", 4242 };
  g_assert_true(pt_agent_session_report_matches(&r, PT_AGENT_CLAUDE, 4242));
  g_assert_false(pt_agent_session_report_matches(&r, PT_AGENT_CLAUDE, 4243));
  g_assert_false(pt_agent_session_report_matches(&r, PT_AGENT_CODEX, 4242));
  g_assert_false(pt_agent_session_report_matches(&r, PT_AGENT_NONE, 0));
}

static void test_resume_command(void) {
  char *c = pt_agent_session_resume_command(PT_AGENT_CLAUDE, "abc-123");
  g_assert_cmpstr(c, ==, "claude --resume 'abc-123'\n");
  g_free(c);
  c = pt_agent_session_resume_command(PT_AGENT_CODEX, "abc-123");
  g_assert_cmpstr(c, ==, "codex resume 'abc-123'\n");
  g_free(c);
  /* The quoting is the security boundary: a crafted id must not escape. A
   * shell reading the line back has to see exactly three words, whatever the
   * id contains, with the whole hostile id landing in the third. */
  c = pt_agent_session_resume_command(PT_AGENT_CLAUDE, "x'; rm -rf $HOME;'");
  g_assert_nonnull(c);
  g_assert_nonnull(strstr(c, "--resume "));
  int argc = 0;
  char **argv = NULL;
  g_assert_true(g_shell_parse_argv(c, &argc, &argv, NULL));
  g_assert_cmpint(argc, ==, 3);
  g_assert_cmpstr(argv[0], ==, "claude");
  g_assert_cmpstr(argv[1], ==, "--resume");
  g_assert_cmpstr(argv[2], ==, "x'; rm -rf $HOME;'");
  g_strfreev(argv);
  g_free(c);
  g_assert_null(pt_agent_session_resume_command(PT_AGENT_NONE, "x"));
  g_assert_null(pt_agent_session_resume_command(PT_AGENT_CLAUDE, NULL));
  g_assert_null(pt_agent_session_resume_command(PT_AGENT_CLAUDE, ""));
}

static void test_agent_ancestor(void) {
  /* What is up the chain depends on who started the suite: ctest from a
   * terminal has no agent above it, ctest from inside a coding agent does.
   * So this asserts the contract rather than a fixed answer — a zero says
   * NONE, and a non-zero names a live process whose cmdline really is the
   * agent it reported. */
  PtAgentKind k = PT_AGENT_CLAUDE;
  int pid = pt_agent_session_find_agent_ancestor(&k);
  if (pid == 0) {
    g_assert_cmpint(k, ==, PT_AGENT_NONE);
  } else {
    g_assert_cmpint(k, !=, PT_AGENT_NONE);
    char *path = g_strdup_printf("/proc/%d/cmdline", pid);
    char *buf = NULL;
    gsize len = 0;
    g_assert_true(g_file_get_contents(path, &buf, &len, NULL));
    g_assert_cmpint(pt_agent_kind_from_cmdline(buf, len), ==, k);
    g_free(buf);
    g_free(path);
  }
  /* out_kind is optional: a caller that only wants the pid passes NULL. */
  g_assert_cmpint(pt_agent_session_find_agent_ancestor(NULL), ==, pid);
}

/* PT_AGENT_REPORT_BIN is a compile definition added in CMakeLists.txt. */

/* Runs the helper with an optional extra argument (the notify payload codex
 * passes) and optional stdin text. The two are what tell the modes apart, so
 * one runner covers both rather than two near-copies. */
static void run_helper_full(const char *mode, const char *arg,
                            const char *stdin_text, const char *token,
                            const char *dir, int *exit_code) {
  const char *argv[] = { PT_AGENT_REPORT_BIN, mode, arg, NULL };
  GSubprocessLauncher *l =
      g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE);
  if (token != NULL)
    g_subprocess_launcher_setenv(l, "PT_PANE_TOKEN", token, TRUE);
  else
    g_subprocess_launcher_unsetenv(l, "PT_PANE_TOKEN");
  /* point XDG_STATE_HOME at the test dir so reports land somewhere owned */
  g_subprocess_launcher_setenv(l, "XDG_STATE_HOME", dir, TRUE);
  GError *err = NULL;
  GSubprocess *p = g_subprocess_launcher_spawnv(l, argv, &err);
  g_assert_no_error(err);
  g_assert_true(g_subprocess_communicate_utf8(p, stdin_text, NULL, NULL,
                                              NULL, &err));
  g_assert_no_error(err);
  g_assert_true(g_subprocess_get_if_exited(p));
  *exit_code = g_subprocess_get_exit_status(p);
  g_object_unref(p); g_object_unref(l);
}

static void run_helper(const char *mode, const char *stdin_text,
                       const char *token, const char *dir, int *exit_code) {
  run_helper_full(mode, NULL, stdin_text, token, dir, exit_code);
}

static void test_helper_claude_writes_report(void) {
  char *dir = g_dir_make_tmp("pt-helper-XXXXXX", NULL);
  int code = -1;
  run_helper("claude",
      "{\"session_id\":\"abc-123\",\"cwd\":\"/tmp/x\","
      "\"hook_event_name\":\"SessionStart\",\"source\":\"startup\"}",
      "feedfacefeedface", dir, &code);
  g_assert_cmpint(code, ==, 0);
  char *path = g_build_filename(dir, "pt", "agent-sessions",
                                "feedfacefeedface.json", NULL);
  PtAgentSessionReport *r = pt_agent_session_report_load(path);
  g_assert_nonnull(r);
  g_assert_cmpint(r->agent, ==, PT_AGENT_CLAUDE);
  g_assert_cmpstr(r->session_id, ==, "abc-123");
  /* no agent in the test's ancestry: the helper records its own parent's pid
   * as a best effort? No — the contract is agent pid or the reporting
   * process's ppid fallback; assert it recorded something > 0. */
  g_assert_cmpint(r->pid, >, 0);
  pt_agent_session_report_free(r);
  g_free(path); g_free(dir);   /* leak the tree; it is /tmp and test-scoped */
}

/* codex hands its notify program the payload as one argv word and closes
 * stdin, and names the conversation "thread-id" — the string `codex resume`
 * takes. Both differences from claude live in the helper, so both are here. */
static void test_helper_codex_writes_report(void) {
  char *dir = g_dir_make_tmp("pt-helper-XXXXXX", NULL);
  int code = -1;
  run_helper_full("codex-notify",
      "{\"type\":\"agent-turn-complete\","
      "\"thread-id\":\"019fdd5e-918f-7aa1-9843-a59fe0fa012c\","
      "\"turn-id\":\"t1\",\"cwd\":\"/tmp/y\",\"client\":\"codex_exec\"}",
      NULL, "cafecafecafecafe", dir, &code);
  g_assert_cmpint(code, ==, 0);
  char *path = g_build_filename(dir, "pt", "agent-sessions",
                                "cafecafecafecafe.json", NULL);
  PtAgentSessionReport *r = pt_agent_session_report_load(path);
  g_assert_nonnull(r);
  g_assert_cmpint(r->agent, ==, PT_AGENT_CODEX);
  g_assert_cmpstr(r->session_id, ==, "019fdd5e-918f-7aa1-9843-a59fe0fa012c");
  g_assert_cmpstr(r->cwd, ==, "/tmp/y");
  g_assert_cmpint(r->pid, >, 0);
  pt_agent_session_report_free(r);
  g_free(path); g_free(dir);
}

static void test_helper_no_token_is_noop(void) {
  char *dir = g_dir_make_tmp("pt-helper-XXXXXX", NULL);
  int code = -1;
  run_helper("claude", "{\"session_id\":\"abc\"}", NULL, dir, &code);
  g_assert_cmpint(code, ==, 0);
  char *sessions = g_build_filename(dir, "pt", "agent-sessions", NULL);
  g_assert_false(g_file_test(sessions, G_FILE_TEST_EXISTS));
  g_free(sessions); g_free(dir);
}

static void test_helper_no_session_id_is_noop(void) {
  char *dir = g_dir_make_tmp("pt-helper-XXXXXX", NULL);
  int code = -1;
  run_helper("claude", "{\"hook_event_name\":\"SessionStart\"}",
             "feedfacefeedface", dir, &code);
  g_assert_cmpint(code, ==, 0);
  char *path = g_build_filename(dir, "pt", "agent-sessions",
                                "feedfacefeedface.json", NULL);
  g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
  g_free(path); g_free(dir);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/agent-session/kind-names", test_kind_names);
  g_test_add_func("/agent-session/token", test_token);
  g_test_add_func("/agent-session/report-roundtrip", test_report_roundtrip);
  g_test_add_func("/agent-session/report-rejects", test_report_load_rejects);
  g_test_add_func("/agent-session/matches", test_report_matches);
  g_test_add_func("/agent-session/resume-command", test_resume_command);
  g_test_add_func("/agent-session/ancestor", test_agent_ancestor);
  g_test_add_func("/agent-session/helper-claude", test_helper_claude_writes_report);
  g_test_add_func("/agent-session/helper-codex", test_helper_codex_writes_report);
  g_test_add_func("/agent-session/helper-no-token", test_helper_no_token_is_noop);
  g_test_add_func("/agent-session/helper-no-session-id",
                  test_helper_no_session_id_is_noop);
  return g_test_run();
}
