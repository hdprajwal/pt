#include "pt-integration.h"
#include <json-glib/json-glib.h>
#include <string.h>

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

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/integration/merge-empty", test_merge_into_empty);
  g_test_add_func("/integration/merge-preserves", test_merge_preserves_existing);
  g_test_add_func("/integration/merge-idempotent", test_merge_idempotent);
  g_test_add_func("/integration/merge-refuses", test_merge_refuses_malformed);
  g_test_add_func("/integration/installed-no", test_installed_answers_no);
  return g_test_run();
}
