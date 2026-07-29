#include "pt-split-tree.h"
#include <string.h>

PtSplitNode *pt_split_leaf_new(const char *cwd) {
  PtSplitNode *n = g_new0(PtSplitNode, 1);
  n->kind = PT_SPLIT_LEAF;
  n->cwd = g_strdup(cwd != NULL ? cwd : g_get_home_dir());
  return n;
}

static void replace_child(PtSplitNode *parent, PtSplitNode *old,
                          PtSplitNode *newc) {
  if (parent->a == old) parent->a = newc;
  else parent->b = newc;
  newc->parent = parent;
}

PtSplitNode *pt_split_split(PtSplitNode **root, PtSplitNode *leaf,
                            PtSplitKind kind) {
  g_return_val_if_fail(leaf->kind == PT_SPLIT_LEAF, NULL);
  PtSplitNode *old_parent = leaf->parent;
  PtSplitNode *split = g_new0(PtSplitNode, 1);
  PtSplitNode *fresh = pt_split_leaf_new(leaf->cwd);
  split->kind = kind;
  split->ratio = 0.5;
  split->a = leaf;
  split->b = fresh;
  split->parent = old_parent;
  if (old_parent != NULL) replace_child(old_parent, leaf, split);
  else *root = split;
  leaf->parent = split;
  fresh->parent = split;
  return fresh;
}

PtSplitNode *pt_split_close(PtSplitNode **root, PtSplitNode *leaf) {
  g_return_val_if_fail(leaf->kind == PT_SPLIT_LEAF, NULL);
  PtSplitNode *parent = leaf->parent;
  if (parent == NULL) { /* closing the only pane */
    pt_split_free(leaf);
    *root = NULL;
    return NULL;
  }
  PtSplitNode *sibling = (parent->a == leaf) ? parent->b : parent->a;
  sibling->parent = parent->parent;
  if (parent->parent != NULL) replace_child(parent->parent, parent, sibling);
  else *root = sibling;
  g_free(leaf->cwd);
  g_free(leaf);
  g_free(parent->cwd);
  g_free(parent);
  return pt_split_first_leaf(sibling);
}

PtSplitNode *pt_split_first_leaf(PtSplitNode *root) {
  if (root == NULL) return NULL;
  while (root->kind != PT_SPLIT_LEAF) root = root->a;
  return root;
}

static PtSplitNode *last_leaf(PtSplitNode *n) {
  while (n->kind != PT_SPLIT_LEAF) n = n->b;
  return n;
}

/* O(depth), no allocation: climb while we are the second child, then the
 * neighbor is the first leaf under the parent's other side; climbing off the
 * root means `leaf` was the last leaf, so wrap to the first. */
PtSplitNode *pt_split_next_leaf(PtSplitNode *root, PtSplitNode *leaf) {
  if (root == NULL || leaf == NULL) return leaf;
  PtSplitNode *n = leaf;
  while (n->parent != NULL && n->parent->b == n) n = n->parent;
  if (n->parent == NULL) return pt_split_first_leaf(root); /* wrap */
  return pt_split_first_leaf(n->parent->b);
}

PtSplitNode *pt_split_prev_leaf(PtSplitNode *root, PtSplitNode *leaf) {
  if (root == NULL || leaf == NULL) return leaf;
  PtSplitNode *n = leaf;
  while (n->parent != NULL && n->parent->a == n) n = n->parent;
  if (n->parent == NULL) return last_leaf(root); /* wrap */
  return last_leaf(n->parent->a);
}

PtSplitNode *pt_split_copy(const PtSplitNode *n) {
  if (n == NULL) return NULL;
  PtSplitNode *c = g_new0(PtSplitNode, 1); /* user stays NULL by design */
  c->kind = n->kind;
  c->ratio = n->ratio;
  c->cwd = g_strdup(n->cwd);
  c->a = pt_split_copy(n->a);
  c->b = pt_split_copy(n->b);
  if (c->a != NULL) c->a->parent = c;
  if (c->b != NULL) c->b->parent = c;
  return c;
}

int pt_split_count_leaves(PtSplitNode *root) {
  if (root == NULL) return 0;
  if (root->kind == PT_SPLIT_LEAF) return 1;
  return pt_split_count_leaves(root->a) + pt_split_count_leaves(root->b);
}

void pt_split_free(PtSplitNode *root) {
  if (root == NULL) return;
  if (root->kind != PT_SPLIT_LEAF) {
    pt_split_free(root->a);
    pt_split_free(root->b);
  }
  g_free(root->cwd);
  g_free(root);
}

JsonNode *pt_split_to_json(const PtSplitNode *root) {
  JsonObject *obj = json_object_new();
  if (root->kind == PT_SPLIT_LEAF) {
    json_object_set_string_member(obj, "kind", "leaf");
    json_object_set_string_member(obj, "cwd", root->cwd);
  } else {
    json_object_set_string_member(obj, "kind",
                                  root->kind == PT_SPLIT_H ? "h" : "v");
    json_object_set_double_member(obj, "ratio", root->ratio);
    json_object_set_member(obj, "a", pt_split_to_json(root->a));
    json_object_set_member(obj, "b", pt_split_to_json(root->b));
  }
  JsonNode *node = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(node, obj);
  return node;
}

PtSplitNode *pt_split_from_json(JsonNode *node) {
  if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node)) return NULL;
  JsonObject *obj = json_node_get_object(node);
  const char *kind = json_object_get_string_member_with_default(obj, "kind", "");
  if (g_strcmp0(kind, "leaf") == 0) {
    return pt_split_leaf_new(
        json_object_get_string_member_with_default(obj, "cwd", NULL));
  }
  if (g_strcmp0(kind, "h") != 0 && g_strcmp0(kind, "v") != 0) return NULL;
  PtSplitNode *a = pt_split_from_json(json_object_get_member(obj, "a"));
  PtSplitNode *b = pt_split_from_json(json_object_get_member(obj, "b"));
  if (a == NULL || b == NULL) {
    pt_split_free(a);
    pt_split_free(b);
    return NULL;
  }
  PtSplitNode *n = g_new0(PtSplitNode, 1);
  n->kind = (kind[0] == 'h') ? PT_SPLIT_H : PT_SPLIT_V;
  n->ratio = json_object_get_double_member_with_default(obj, "ratio", 0.5);
  n->a = a;
  n->b = b;
  a->parent = n;
  b->parent = n;
  return n;
}
