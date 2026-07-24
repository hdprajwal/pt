#include "pt-split-tree.h"

static void test_single_leaf(void) {
  PtSplitNode *root = pt_split_leaf_new("/tmp");
  g_assert_cmpint(pt_split_count_leaves(root), ==, 1);
  g_assert_true(pt_split_first_leaf(root) == root);
  g_assert_true(pt_split_next_leaf(root, root) == root);
  pt_split_free(root);
}

static void test_split_and_close(void) {
  PtSplitNode *root = pt_split_leaf_new("/a");
  PtSplitNode *first = root;
  PtSplitNode *second = pt_split_split(&root, first, PT_SPLIT_H);
  g_assert_cmpint(pt_split_count_leaves(root), ==, 2);
  g_assert_true(root->kind == PT_SPLIT_H);
  g_assert_true(root->a == first && root->b == second);
  g_assert_cmpstr(second->cwd, ==, "/a");
  g_assert_true(pt_split_next_leaf(root, first) == second);
  g_assert_true(pt_split_next_leaf(root, second) == first); /* cyclic */

  PtSplitNode *focus = pt_split_close(&root, second);
  g_assert_true(focus == first);
  g_assert_true(root == first);            /* collapsed back to a leaf */
  g_assert_cmpint(pt_split_count_leaves(root), ==, 1);
  pt_split_free(root);
}

static void test_close_root_leaf(void) {
  PtSplitNode *root = pt_split_leaf_new("/a");
  g_assert_null(pt_split_close(&root, root));
  g_assert_null(root);
}

static void test_json_roundtrip(void) {
  PtSplitNode *root = pt_split_leaf_new("/a");
  pt_split_split(&root, root, PT_SPLIT_H);
  pt_split_split(&root, root->b, PT_SPLIT_V);
  root->ratio = 0.3;

  JsonNode *j = pt_split_to_json(root);
  PtSplitNode *back = pt_split_from_json(j);
  json_node_unref(j);
  g_assert_nonnull(back);
  g_assert_cmpint(pt_split_count_leaves(back), ==, 3);
  g_assert_true(back->kind == PT_SPLIT_H);
  g_assert_cmpfloat(back->ratio, ==, 0.3);
  g_assert_true(back->b->kind == PT_SPLIT_V);
  g_assert_cmpstr(pt_split_first_leaf(back)->cwd, ==, "/a");
  pt_split_free(root);
  pt_split_free(back);
}

static void test_from_json_malformed(void) {
  JsonNode *n = json_node_new(JSON_NODE_VALUE);
  json_node_set_int(n, 42);
  g_assert_null(pt_split_from_json(n));
  json_node_unref(n);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/split/single", test_single_leaf);
  g_test_add_func("/split/split-close", test_split_and_close);
  g_test_add_func("/split/close-root", test_close_root_leaf);
  g_test_add_func("/split/json", test_json_roundtrip);
  g_test_add_func("/split/malformed", test_from_json_malformed);
  return g_test_run();
}
