#include "pt-agent.h"
#include <string.h>
#include <unistd.h>

/* The names pt is looking for, and the ones that merely start the same way.
 * The negative cases are the point of the exact match: claude-desktop is a
 * different program, and reporting a pane as running Claude Code because the
 * desktop app happens to be open would be worse than showing nothing. */
static void test_names(void) {
  g_assert_cmpint(pt_agent_kind_from_name("claude"), ==, PT_AGENT_CLAUDE);
  g_assert_cmpint(pt_agent_kind_from_name("codex"), ==, PT_AGENT_CODEX);
  g_assert_cmpint(pt_agent_kind_from_name("claude-desktop"), ==,
                  PT_AGENT_NONE);
  g_assert_cmpint(pt_agent_kind_from_name("claudia"), ==, PT_AGENT_NONE);
  g_assert_cmpint(pt_agent_kind_from_name("codexctl"), ==, PT_AGENT_NONE);
  g_assert_cmpint(pt_agent_kind_from_name("zsh"), ==, PT_AGENT_NONE);
  g_assert_cmpint(pt_agent_kind_from_name(""), ==, PT_AGENT_NONE);
  g_assert_cmpint(pt_agent_kind_from_name(NULL), ==, PT_AGENT_NONE);
}

/* A path answers for what it runs. /proc/<pid>/comm is a bare name, but the
 * same matcher is handed cmdline entries and hand-typed commands. */
static void test_paths(void) {
  g_assert_cmpint(pt_agent_kind_from_name("/usr/local/bin/claude"), ==,
                  PT_AGENT_CLAUDE);
  g_assert_cmpint(pt_agent_kind_from_name("~/.local/bin/codex"), ==,
                  PT_AGENT_CODEX);
  /* A trailing slash names a directory, and a directory runs nothing. */
  g_assert_cmpint(pt_agent_kind_from_name("/opt/claude/"), ==, PT_AGENT_NONE);
}

/* A cmdline is NUL-separated, so the cases are written as one buffer. */
static PtAgentKind cmdline(const char *args[], int n) {
  GString *b = g_string_new(NULL);
  for (int i = 0; i < n; i++) g_string_append_len(b, args[i], strlen(args[i]) + 1);
  PtAgentKind k = pt_agent_kind_from_cmdline(b->str, b->len);
  g_string_free(b, TRUE);
  return k;
}

/* The case comm cannot answer: Claude Code installed through npm runs under
 * node, so comm is "node" and the agent's name is only on the command line. */
static void test_cmdline(void) {
  const char *npm[] = { "node", "/home/u/.npm-global/lib/node_modules/"
                                "@anthropic-ai/claude-code/cli.js" };
  /* The script is not named for the agent, so this one is genuinely unknown —
   * argv[1] is what gets looked at, and it says cli.js. */
  g_assert_cmpint(cmdline(npm, 2), ==, PT_AGENT_NONE);

  const char *shim[] = { "node", "/home/u/.local/bin/claude", "--resume" };
  g_assert_cmpint(cmdline(shim, 3), ==, PT_AGENT_CLAUDE);

  /* argv[0] alone answers for a wrapper that exec'd the agent directly. */
  const char *direct[] = { "/usr/local/bin/codex", "exec" };
  g_assert_cmpint(cmdline(direct, 2), ==, PT_AGENT_CODEX);

  /* Past argv[1] is the agent's own arguments. A `claude` there is a word
   * somebody typed, not the program that is running. */
  const char *arg[] = { "grep", "-r", "claude", "src/" };
  g_assert_cmpint(cmdline(arg, 4), ==, PT_AGENT_NONE);
  const char *editing[] = { "nvim", "notes.md", "codex" };
  g_assert_cmpint(cmdline(editing, 3), ==, PT_AGENT_NONE);

  /* /proc hands back whatever it has: an empty cmdline (a kernel thread), and
   * a last argument whose terminator falls outside the buffer. */
  g_assert_cmpint(pt_agent_kind_from_cmdline("", 0), ==, PT_AGENT_NONE);
  g_assert_cmpint(pt_agent_kind_from_cmdline(NULL, 12), ==, PT_AGENT_NONE);
  g_assert_cmpint(pt_agent_kind_from_cmdline("node\0/bin/claude", 16), ==,
                  PT_AGENT_CLAUDE);
}

static void test_labels(void) {
  g_assert_cmpstr(pt_agent_label(PT_AGENT_CLAUDE), ==, "Claude Code");
  g_assert_cmpstr(pt_agent_label(PT_AGENT_CODEX), ==, "Codex");
  g_assert_cmpstr(pt_agent_label(PT_AGENT_NONE), ==, "");
}

/* The fast path answers without a pid at all, which is what makes it free
 * enough to run on the panel's twice-a-second refresh. */
static void test_detect_foreground(void) {
  int pid = -1;
  g_assert_cmpint(pt_agent_detect(0, "codex", &pid), ==, PT_AGENT_CODEX);
  g_assert_cmpint(pid, ==, 0);
}

/* No shell, nothing running: the walk is skipped and the answer is none. This
 * is the state a pane is in almost all the time. */
static void test_detect_nothing(void) {
  int pid = -1;
  g_assert_cmpint(pt_agent_detect(0, "zsh", &pid), ==, PT_AGENT_NONE);
  g_assert_cmpint(pid, ==, 0);
  g_assert_cmpint(pt_agent_detect(-1, NULL, NULL), ==, PT_AGENT_NONE);
}

/* The walk over a real process tree: this test's own pid has no agent under
 * it, and asking must terminate rather than wander off into /proc. */
static void test_detect_walk_self(void) {
  int pid = -1;
  g_assert_cmpint(pt_agent_detect((int)getpid(), NULL, &pid), ==,
                  PT_AGENT_NONE);
  g_assert_cmpint(pid, ==, 0);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/agent/names", test_names);
  g_test_add_func("/agent/paths", test_paths);
  g_test_add_func("/agent/cmdline", test_cmdline);
  g_test_add_func("/agent/labels", test_labels);
  g_test_add_func("/agent/detect-foreground", test_detect_foreground);
  g_test_add_func("/agent/detect-nothing", test_detect_nothing);
  g_test_add_func("/agent/detect-walk-self", test_detect_walk_self);
  return g_test_run();
}
