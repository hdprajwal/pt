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

/* 5-leaf nested tree used by the navigation tests:
 *   H( V(L1, L4), V(L2, H(L3, L5)) )
 * In-order leaves: L1, L4, L2, L3, L5. */
static PtSplitNode *build_five_leaf(PtSplitNode *order[5]) {
  PtSplitNode *root = pt_split_leaf_new("/a");
  PtSplitNode *l1 = root;
  PtSplitNode *l2 = pt_split_split(&root, l1, PT_SPLIT_H);
  PtSplitNode *l3 = pt_split_split(&root, l2, PT_SPLIT_V);
  PtSplitNode *l4 = pt_split_split(&root, l1, PT_SPLIT_V);
  PtSplitNode *l5 = pt_split_split(&root, l3, PT_SPLIT_H);
  order[0] = l1;
  order[1] = l4;
  order[2] = l2;
  order[3] = l3;
  order[4] = l5;
  return root;
}

static void test_next_leaf_table(void) {
  PtSplitNode *order[5];
  PtSplitNode *root = build_five_leaf(order);
  g_assert_cmpint(pt_split_count_leaves(root), ==, 5);
  for (int i = 0; i < 5; i++) {
    g_assert_true(pt_split_first_leaf(root) == order[0]);
    g_assert_true(pt_split_next_leaf(root, order[i]) == order[(i + 1) % 5]);
  }
  pt_split_free(root);
}

static void test_prev_leaf_table(void) {
  PtSplitNode *order[5];
  PtSplitNode *root = build_five_leaf(order);
  for (int i = 0; i < 5; i++)
    g_assert_true(pt_split_prev_leaf(root, order[i]) == order[(i + 4) % 5]);
  pt_split_free(root);

  /* prev must mirror next's wraparound on the degenerate trees too */
  PtSplitNode *lone = pt_split_leaf_new("/tmp");
  g_assert_true(pt_split_prev_leaf(lone, lone) == lone);
  pt_split_free(lone);

  PtSplitNode *pair = pt_split_leaf_new("/a");
  PtSplitNode *first = pair;
  PtSplitNode *second = pt_split_split(&pair, first, PT_SPLIT_H);
  g_assert_true(pt_split_prev_leaf(pair, first) == second);  /* cyclic */
  g_assert_true(pt_split_prev_leaf(pair, second) == first);
  pt_split_free(pair);
}

static void test_next_prev_inverse(void) {
  PtSplitNode *order[5];
  PtSplitNode *root = build_five_leaf(order);
  for (int i = 0; i < 5; i++) {
    g_assert_true(pt_split_prev_leaf(root, pt_split_next_leaf(root, order[i]))
                  == order[i]);
    g_assert_true(pt_split_next_leaf(root, pt_split_prev_leaf(root, order[i]))
                  == order[i]);
  }
  pt_split_free(root);
}

static void test_copy(void) {
  PtSplitNode *order[5];
  PtSplitNode *root = build_five_leaf(order);
  root->ratio = 0.3;
  order[0]->user = (void *)0xdeadbeef; /* must NOT be copied */
  g_free(order[3]->cwd);
  order[3]->cwd = g_strdup("/deep");

  PtSplitNode *copy = pt_split_copy(root);
  g_assert_nonnull(copy);
  g_assert_true(copy != root);
  g_assert_cmpint(pt_split_count_leaves(copy), ==, 5);
  g_assert_true(copy->kind == PT_SPLIT_H);
  g_assert_cmpfloat(copy->ratio, ==, 0.3);
  g_assert_null(copy->parent);
  g_assert_true(copy->a->parent == copy && copy->b->parent == copy);

  /* leaves match in order, cwds copied, user always NULL */
  PtSplitNode *cl = pt_split_first_leaf(copy);
  for (int i = 0; i < 5; i++) {
    g_assert_true(cl != order[i]);
    g_assert_true(cl->kind == PT_SPLIT_LEAF);
    g_assert_cmpstr(cl->cwd, ==, order[i]->cwd);
    g_assert_null(cl->user);
    cl = pt_split_next_leaf(copy, cl);
  }
  g_assert_true(cl == pt_split_first_leaf(copy)); /* wrapped exactly once */

  /* mutate the copy; the original must not move */
  PtSplitNode *orig_l3 = order[3];
  PtSplitNode *copy_first = pt_split_first_leaf(copy);
  g_free(copy_first->cwd);
  copy_first->cwd = g_strdup("/mutated");
  copy->ratio = 0.9;
  pt_split_close(&copy, pt_split_first_leaf(copy));
  g_assert_cmpint(pt_split_count_leaves(copy), ==, 4);
  g_assert_cmpint(pt_split_count_leaves(root), ==, 5);
  g_assert_cmpfloat(root->ratio, ==, 0.3);
  g_assert_cmpstr(order[0]->cwd, ==, "/a");
  g_assert_cmpstr(orig_l3->cwd, ==, "/deep");
  g_assert_true(order[0]->user == (void *)0xdeadbeef);

  order[0]->user = NULL;
  pt_split_free(copy);
  pt_split_free(root);
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

static void test_agent_fields_json(void) {
  PtSplitNode *root = pt_split_leaf_new("/a");
  pt_split_leaf_set_agent(root, "claude", "abc-123");
  PtSplitNode *second = pt_split_split(&root, root, PT_SPLIT_H);
  /* a split's fresh leaf is a new shell, never an inherited conversation */
  g_assert_null(second->agent);
  JsonNode *j = pt_split_to_json(root);
  PtSplitNode *back = pt_split_from_json(j);
  json_node_unref(j);
  PtSplitNode *first = pt_split_first_leaf(back);
  g_assert_cmpstr(first->agent, ==, "claude");
  g_assert_cmpstr(first->agent_session, ==, "abc-123");
  g_assert_null(pt_split_next_leaf(back, first)->agent);
  pt_split_free(back); pt_split_free(root);
}

static void test_agent_fields_copy_and_strip(void) {
  PtSplitNode *root = pt_split_leaf_new("/a");
  pt_split_leaf_set_agent(root, "codex", "id-9");
  PtSplitNode *copy = pt_split_copy(root);
  g_assert_cmpstr(copy->agent, ==, "codex");
  pt_split_strip_agents(copy);
  g_assert_null(copy->agent);
  g_assert_null(copy->agent_session);
  /* set NULL clears */
  pt_split_leaf_set_agent(root, NULL, NULL);
  g_assert_null(root->agent);
  pt_split_free(copy); pt_split_free(root);
}

static void test_agent_fields_absent_in_old_json(void) {
  /* a v1 file's leaves have no agent members; they must load as NULL */
  JsonParser *p = json_parser_new();
  g_assert_true(json_parser_load_from_data(p,
      "{\"kind\":\"leaf\",\"cwd\":\"/x\"}", -1, NULL));
  PtSplitNode *n = pt_split_from_json(json_parser_get_root(p));
  g_assert_nonnull(n);
  g_assert_null(n->agent);
  pt_split_free(n); g_object_unref(p);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/split/single", test_single_leaf);
  g_test_add_func("/split/split-close", test_split_and_close);
  g_test_add_func("/split/close-root", test_close_root_leaf);
  g_test_add_func("/split/next-leaf-table", test_next_leaf_table);
  g_test_add_func("/split/prev-leaf-table", test_prev_leaf_table);
  g_test_add_func("/split/next-prev-inverse", test_next_prev_inverse);
  g_test_add_func("/split/copy", test_copy);
  g_test_add_func("/split/json", test_json_roundtrip);
  g_test_add_func("/split/malformed", test_from_json_malformed);
  g_test_add_func("/split/agent-json", test_agent_fields_json);
  g_test_add_func("/split/agent-copy-strip", test_agent_fields_copy_and_strip);
  g_test_add_func("/split/agent-absent", test_agent_fields_absent_in_old_json);
  return g_test_run();
}
