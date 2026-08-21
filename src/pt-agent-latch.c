#include "pt-agent-latch.h"

struct _PtAgentLatch {
  /* token → strdup'd name of the last event a notification was raised for */
  GHashTable *last;
};

PtAgentLatch *pt_agent_latch_new(void) {
  PtAgentLatch *l = g_new0(PtAgentLatch, 1);
  l->last = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  return l;
}

void pt_agent_latch_free(PtAgentLatch *l) {
  if (l == NULL) return;
  g_clear_pointer(&l->last, g_hash_table_unref);
  g_free(l);
}

gboolean pt_agent_latch_should_notify(PtAgentLatch *l, const char *token,
                                      const char *event) {
  const char *last = g_hash_table_lookup(l->last, token);
  return g_strcmp0(last, event) != 0;
}

void pt_agent_latch_record(PtAgentLatch *l, const char *token,
                           const char *event) {
  g_hash_table_replace(l->last, g_strdup(token), g_strdup(event));
}

void pt_agent_latch_rearm(PtAgentLatch *l, const char *token) {
  g_hash_table_remove(l->last, token);
}

void pt_agent_latch_prune(PtAgentLatch *l, const char *const *live,
                          gsize n_live) {
  /* Removing from l->last during its own foreach would be UB, so the tokens
   * to drop are collected first and removed in a second pass. */
  GHashTable *dead = g_hash_table_new(g_str_hash, g_str_equal);
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, l->last);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    gboolean found = FALSE;
    for (gsize i = 0; i < n_live && !found; i++)
      found = g_strcmp0(live[i], (const char *)k) == 0;
    if (!found) g_hash_table_add(dead, k);
  }
  g_hash_table_iter_init(&it, dead);
  while (g_hash_table_iter_next(&it, &k, &v))
    g_hash_table_remove(l->last, k);
  g_hash_table_unref(dead);
}

guint pt_agent_latch_count(PtAgentLatch *l) {
  return g_hash_table_size(l->last);
}
