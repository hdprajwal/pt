#include "pt-integration.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#define BIN "/usr/local/bin/pt"

static void test_merge_into_empty(void) {
  GError *err = NULL;
  char *out = pt_integration_claude_merged_settings(NULL, BIN, &err);
  g_assert_no_error(err);
  g_assert_nonnull(out);
  g_assert_true(pt_integration_claude_installed(out, BIN));
  /* and the result is valid JSON with the right shape under each key */
  JsonParser *p = json_parser_new();
  g_assert_true(json_parser_load_from_data(p, out, -1, NULL));
  JsonObject *root = json_node_get_object(json_parser_get_root(p));
  JsonObject *hooks = json_object_get_object_member(root, "hooks");
  const char *events[] = { "SessionStart", "Stop", "Notification" };
  for (gsize i = 0; i < G_N_ELEMENTS(events); i++) {
    JsonArray *arr = json_object_get_array_member(hooks, events[i]);
    g_assert_cmpuint(json_array_get_length(arr), ==, 1);
  }
  /* the lifecycle hooks run the claude-event mode with their event name */
  g_assert_nonnull(strstr(out, "agent-report claude-event Stop"));
  g_assert_nonnull(strstr(out, "agent-report claude-event Notification"));
  g_object_unref(p); g_free(out);
}

static void test_merge_preserves_existing(void) {
  const char *existing =
      "{\"model\":\"opus\",\"hooks\":{\"SessionStart\":["
      "{\"hooks\":[{\"type\":\"command\",\"command\":\"other-hook\"}]}],"
      "\"PostToolUse\":[]}}";
  GError *err = NULL;
  char *out = pt_integration_claude_merged_settings(existing, BIN, &err);
  g_assert_no_error(err);
  g_assert_nonnull(strstr(out, "other-hook"));
  g_assert_nonnull(strstr(out, "\"model\""));
  g_assert_nonnull(strstr(out, "PostToolUse"));
  g_assert_true(pt_integration_claude_installed(out, BIN));
  g_free(out);
}

/* A settings.json written by an older pt has SessionStart and nothing else.
 * The merge must keep that entry untouched and add only the missing keys —
 * not re-append a second SessionStart hook. */
static void test_merge_upgrades_old_install(void) {
  char *old_cmd = g_strconcat(BIN, " agent-report claude", NULL);
  char *old_json = g_strdup_printf(
      "{\"hooks\":{\"SessionStart\":[{\"hooks\":[{\"type\":\"command\","
      "\"command\":\"%s\"}]}]}}", old_cmd);
  GError *err = NULL;
  char *out = pt_integration_claude_merged_settings(old_json, BIN, &err);
  g_assert_no_error(err);
  JsonParser *p = json_parser_new();
  g_assert_true(json_parser_load_from_data(p, out, -1, NULL));
  JsonObject *hooks = json_object_get_object_member(
      json_node_get_object(json_parser_get_root(p)), "hooks");
  g_assert_cmpuint(json_array_get_length(
                       json_object_get_array_member(hooks, "SessionStart")),
                   ==, 1);
  g_assert_true(json_object_has_member(hooks, "Stop"));
  g_assert_true(json_object_has_member(hooks, "Notification"));
  g_assert_true(pt_integration_claude_installed(out, BIN));
  g_object_unref(p);
  g_free(out); g_free(old_json); g_free(old_cmd);
}

static void test_merge_idempotent(void) {
  GError *err = NULL;
  char *once = pt_integration_claude_merged_settings(NULL, BIN, &err);
  char *twice = pt_integration_claude_merged_settings(once, BIN, &err);
  g_assert_no_error(err);
  g_assert_cmpstr(once, ==, twice);
  g_free(once); g_free(twice);
}

static void test_merge_refuses_malformed(void) {
  GError *err = NULL;
  g_assert_null(pt_integration_claude_merged_settings("{oops", BIN, &err));
  g_assert_nonnull(err);
  g_clear_error(&err);
}

/* The generalized refusal: any event key holding something other than the
 * array-of-entries shape is left alone, whichever event it is — including
 * one pt itself never touches. */
static void test_merge_refuses_bad_event_shape(void) {
  const char *cases[] = {
    "{\"hooks\":{\"Stop\":\"weekly\"}}",
    "{\"hooks\":{\"Notification\":42}}",
    "{\"hooks\":\"all of it\"}",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    GError *err = NULL;
    g_assert_null(pt_integration_claude_merged_settings(cases[i], BIN, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(err->message, "shape"));
    g_clear_error(&err);
  }
}

static void test_installed_answers_no(void) {
  g_assert_false(pt_integration_claude_installed(NULL, BIN));
  g_assert_false(pt_integration_claude_installed("{}", BIN));
}

/* The per-event question is what status prints, so a half-installed file —
 * an older SessionStart-only merge, say — must read as installed for the
 * events it has and not for the ones it lacks. */
static void test_event_installed_per_key(void) {
  const char *text =
      "{\"hooks\":{\"SessionStart\":[{\"hooks\":[{\"type\":\"command\","
      "\"command\":\"" BIN " agent-report claude\"}]}]}}";
  g_assert_true(pt_integration_claude_event_installed(text, BIN,
                                                      "SessionStart"));
  g_assert_false(pt_integration_claude_event_installed(text, BIN, "Stop"));
  g_assert_false(pt_integration_claude_event_installed(text, BIN,
                                                       "Notification"));
  /* unknown event names answer no rather than crashing */
  g_assert_false(pt_integration_claude_event_installed(text, BIN,
                                                       "PreToolUse"));
  g_assert_false(pt_integration_claude_event_installed(NULL, BIN, "Stop"));
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
  g_test_add_func("/integration/merge-upgrades-old",
                  test_merge_upgrades_old_install);
  g_test_add_func("/integration/merge-idempotent", test_merge_idempotent);
  g_test_add_func("/integration/merge-refuses", test_merge_refuses_malformed);
  g_test_add_func("/integration/merge-refuses-bad-shape",
                  test_merge_refuses_bad_event_shape);
  g_test_add_func("/integration/installed-no", test_installed_answers_no);
  g_test_add_func("/integration/event-installed-per-key",
                  test_event_installed_per_key);
  g_test_add_func("/integration/cli-refuses-unreadable",
                  test_cli_refuses_unreadable_settings);
  int rc = g_test_run();
  g_rmdir(home);   /* only when empty; a leftover says a test wrote elsewhere */
  g_free(home);
  return rc;
}
