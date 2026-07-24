#include "pt-session.h"

static PtSessionState *sample_state(void) {
  PtSessionState *s = pt_session_state_new();
  PtProjectState *p = pt_project_state_new("proj", "/tmp/proj");
  PtSplitNode *tree = pt_split_leaf_new("/tmp/proj");
  pt_split_split(&tree, tree, PT_SPLIT_H);
  g_ptr_array_add(p->tabs, pt_tab_state_new("build", tree));
  g_ptr_array_add(p->tabs, pt_tab_state_new("agent",
                  pt_split_leaf_new("/tmp/proj/sub")));
  p->active_tab = 1;
  g_ptr_array_add(s->projects, p);
  s->active_project = 0;
  return s;
}

static void test_roundtrip(void) {
  PtSessionState *s = sample_state();
  char *text = pt_session_to_json_text(s);
  PtSessionState *back = pt_session_from_json_text(text);
  g_free(text);
  g_assert_nonnull(back);
  g_assert_cmpuint(back->projects->len, ==, 1);
  PtProjectState *p = g_ptr_array_index(back->projects, 0);
  g_assert_cmpstr(p->name, ==, "proj");
  g_assert_cmpstr(p->path, ==, "/tmp/proj");
  g_assert_cmpuint(p->tabs->len, ==, 2);
  g_assert_cmpint(p->active_tab, ==, 1);
  PtTabState *t0 = g_ptr_array_index(p->tabs, 0);
  g_assert_cmpstr(t0->title, ==, "build");
  g_assert_cmpint(pt_split_count_leaves(t0->tree), ==, 2);
  pt_session_state_free(s);
  pt_session_state_free(back);
}

static void test_save_load(void) {
  char *dir = g_dir_make_tmp("pt-test-XXXXXX", NULL);
  char *path = g_build_filename(dir, "state.json", NULL);
  PtSessionState *s = sample_state();
  GError *err = NULL;
  g_assert_true(pt_session_save(s, path, &err));
  g_assert_no_error(err);
  PtSessionState *back = pt_session_load(path);
  g_assert_nonnull(back);
  g_assert_cmpuint(back->projects->len, ==, 1);
  pt_session_state_free(s);
  pt_session_state_free(back);
  g_free(path);
  g_free(dir);
}

static void test_corrupt_becomes_bak(void) {
  char *dir = g_dir_make_tmp("pt-test-XXXXXX", NULL);
  char *path = g_build_filename(dir, "state.json", NULL);
  g_file_set_contents(path, "{not json!!", -1, NULL);
  g_assert_null(pt_session_load(path));
  char *bak = g_strconcat(path, ".bak", NULL);
  g_assert_true(g_file_test(bak, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
  g_free(bak); g_free(path); g_free(dir);
}

static void test_load_missing_returns_null(void) {
  g_assert_null(pt_session_load("/nonexistent/dir/state.json"));
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/session/roundtrip", test_roundtrip);
  g_test_add_func("/session/save-load", test_save_load);
  g_test_add_func("/session/corrupt", test_corrupt_becomes_bak);
  g_test_add_func("/session/missing", test_load_missing_returns_null);
  return g_test_run();
}
