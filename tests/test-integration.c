#include "pt-integration.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#define CMD "/usr/local/bin/pt-agent-report claude"

static void test_merge_into_empty(void) {
  GError *err = NULL;
  char *out = pt_integration_claude_merged_settings(NULL, CMD, &err);
  g_assert_no_error(err);
  g_assert_nonnull(out);
  g_assert_true(pt_integration_claude_installed(out, CMD));
  /* and the result is valid JSON with the right shape */
  JsonParser *p = json_parser_new();
  g_assert_true(json_parser_load_from_data(p, out, -1, NULL));
  JsonObject *root = json_node_get_object(json_parser_get_root(p));
  JsonArray *ss = json_object_get_array_member(
      json_object_get_object_member(root, "hooks"), "SessionStart");
  g_assert_cmpuint(json_array_get_length(ss), ==, 1);
  g_object_unref(p); g_free(out);
}

static void test_merge_preserves_existing(void) {
  const char *existing =
      "{\"model\":\"opus\",\"hooks\":{\"SessionStart\":["
      "{\"hooks\":[{\"type\":\"command\",\"command\":\"other-hook\"}]}],"
      "\"PostToolUse\":[]}}";
  GError *err = NULL;
  char *out = pt_integration_claude_merged_settings(existing, CMD, &err);
  g_assert_no_error(err);
  g_assert_nonnull(strstr(out, "other-hook"));
  g_assert_nonnull(strstr(out, "\"model\""));
  g_assert_nonnull(strstr(out, "PostToolUse"));
  g_assert_true(pt_integration_claude_installed(out, CMD));
  g_free(out);
}

static void test_merge_idempotent(void) {
  GError *err = NULL;
  char *once = pt_integration_claude_merged_settings(NULL, CMD, &err);
  char *twice = pt_integration_claude_merged_settings(once, CMD, &err);
  g_assert_no_error(err);
  g_assert_cmpstr(once, ==, twice);
  g_free(once); g_free(twice);
}

static void test_merge_refuses_malformed(void) {
  GError *err = NULL;
  g_assert_null(pt_integration_claude_merged_settings("{oops", CMD, &err));
  g_assert_nonnull(err);
  g_clear_error(&err);
}

static void test_installed_answers_no(void) {
  g_assert_false(pt_integration_claude_installed(NULL, CMD));
  g_assert_false(pt_integration_claude_installed("{}", CMD));
}

/* The CLI must not read an unreadable settings.json as a missing one: that
 * path writes a fresh document, silently replacing whatever the user had.
 * This runs as our own uid, so mode 000 really does deny us — root would sail
 * straight through it, hence the skip. */
static void test_cli_refuses_unreadable_settings(void) {
  if (geteuid() == 0) {
    g_test_skip("running as root, which can read a mode-000 file");
    return;
  }
  char *dir = g_build_filename(g_get_home_dir(), ".claude", NULL);
  g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
  char *path = g_build_filename(dir, "settings.json", NULL);
  const char *original = "{\"model\":\"opus\"}\n";
  g_assert_true(g_file_set_contents(path, original, -1, NULL));
  g_assert_cmpint(g_chmod(path, 0), ==, 0);

  char *argv[] = {(char *)"pt", (char *)"integration", (char *)"install",
                  (char *)"claude", NULL};
  g_assert_cmpint(pt_integration_cli(4, argv), ==, 1);
  /* status refuses too — answering "not installed" would send the user
   * straight back to the command that writes. */
  char *sargv[] = {(char *)"pt", (char *)"integration", (char *)"status", NULL};
  g_assert_cmpint(pt_integration_cli(3, sargv), ==, 1);

  g_assert_cmpint(g_chmod(path, 0600), ==, 0);
  char *after = NULL;
  g_assert_true(g_file_get_contents(path, &after, NULL, NULL));
  g_assert_cmpstr(after, ==, original);

  g_remove(path);
  g_rmdir(dir);
  g_free(after); g_free(path); g_free(dir);
}

int main(int argc, char *argv[]) {
  /* Before anything else: GLib caches the home directory on first use, and
   * the CLI tests must never reach the real ~/.claude/settings.json. */
  char *home = g_dir_make_tmp("pt-integration-home-XXXXXX", NULL);
  g_assert_nonnull(home);
  g_setenv("HOME", home, TRUE);
  g_assert_cmpstr(g_get_home_dir(), ==, home);

  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/integration/merge-empty", test_merge_into_empty);
  g_test_add_func("/integration/merge-preserves", test_merge_preserves_existing);
  g_test_add_func("/integration/merge-idempotent", test_merge_idempotent);
  g_test_add_func("/integration/merge-refuses", test_merge_refuses_malformed);
  g_test_add_func("/integration/installed-no", test_installed_answers_no);
  g_test_add_func("/integration/cli-refuses-unreadable",
                  test_cli_refuses_unreadable_settings);
  int rc = g_test_run();
  g_rmdir(home);   /* only when empty; a leftover says a test wrote elsewhere */
  g_free(home);
  return rc;
}
