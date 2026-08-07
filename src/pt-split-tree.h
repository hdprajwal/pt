#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>

typedef enum { PT_SPLIT_LEAF, PT_SPLIT_H, PT_SPLIT_V } PtSplitKind; /* H = side-by-side */
typedef struct PtSplitNode PtSplitNode;
struct PtSplitNode {
  PtSplitKind kind;
  char *cwd;              /* leaf only */
  void *user;             /* leaf only: owning widget, not serialized */
  double ratio;           /* split only, 0..1 position of divider */
  PtSplitNode *a, *b;     /* split only */
  PtSplitNode *parent;    /* NULL at root */
  /* Leaf only, and both NULL unless an agent reported a session for this pane.
   * They travel together: an agent without a session id cannot be resumed, so
   * neither is written or read on its own. */
  char *agent;            /* machine name, e.g. "claude" */
  char *agent_session;    /* the session id to resume */
};
PtSplitNode *pt_split_leaf_new(const char *cwd);
/* Both strings are copied; passing NULL for both clears the pair. Calling this
 * on a split node is a programmer error. */
void pt_split_leaf_set_agent(PtSplitNode *leaf, const char *agent,
                             const char *session);
/* Clear the agent fields on every leaf under `root` — how a window applies
 * `resume-agents = false` to a tree it is about to restore. */
void pt_split_strip_agents(PtSplitNode *root);
/* Split `leaf` in place: leaf's slot in the tree is taken by a new split node
 * whose `a` is the original leaf and `b` a new leaf with the same cwd.
 * Returns the NEW leaf (b). Updates *root if leaf was the root. */
PtSplitNode *pt_split_split(PtSplitNode **root, PtSplitNode *leaf, PtSplitKind kind);
/* Remove `leaf`; its sibling replaces the parent. Returns the sibling subtree's
 * first leaf (new focus target), or NULL if leaf was the root (tab empty).
 * Updates *root. Frees leaf and the collapsed parent. */
PtSplitNode *pt_split_close(PtSplitNode **root, PtSplitNode *leaf);
PtSplitNode *pt_split_first_leaf(PtSplitNode *root);
PtSplitNode *pt_split_next_leaf(PtSplitNode *root, PtSplitNode *leaf); /* cyclic */
PtSplitNode *pt_split_prev_leaf(PtSplitNode *root, PtSplitNode *leaf); /* cyclic */
/* Deep copy of the whole subtree; `user` pointers are NOT copied (NULL in the
 * copy). Parent pointers are rebuilt; the copy's root parent is NULL. */
PtSplitNode *pt_split_copy(const PtSplitNode *n);
int pt_split_count_leaves(PtSplitNode *root);
void pt_split_free(PtSplitNode *root);
JsonNode *pt_split_to_json(const PtSplitNode *root);       /* transfer full */
PtSplitNode *pt_split_from_json(JsonNode *node);           /* NULL on malformed */
