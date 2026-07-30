#include <glib.h>

#include "pt-workspace.h"
#include "pt-session.h"   /* PT_ACCENT_COUNT */

/* ---------- construction ---------- */

static void test_empty(void) {
  PtWorkspace *ws = pt_workspace_new();
  g_assert_cmpuint(pt_workspace_project_count(ws), ==, 0);
  g_assert_cmpuint(pt_workspace_active_project(ws), ==, PT_WS_ID_NONE);
  g_assert_cmpuint(pt_workspace_project_at(ws, 0), ==, PT_WS_ID_NONE);
  g_assert_cmpuint(pt_workspace_project_index(ws, 1), ==, PT_WS_INDEX_NONE);
  g_assert_cmpuint(pt_workspace_tab_count(ws, 1), ==, 0);
  g_assert_cmpuint(pt_workspace_tab_project(ws, 1), ==, PT_WS_ID_NONE);
  g_assert_null(pt_workspace_get_data(ws, 1));
  g_assert_null(pt_workspace_project_name(ws, 1));
  pt_workspace_free(ws);
  pt_workspace_free(NULL);   /* NULL-safe, like every *_free here */
}

static void test_add_project_basics(void) {
  PtWorkspace *ws = pt_workspace_new();
  PtWsId a = pt_workspace_add_project(ws, "alpha", "/tmp/a", -1);
  PtWsId b = pt_workspace_add_project(ws, "beta", "/tmp/b", -1);
  g_assert_cmpuint(a, !=, PT_WS_ID_NONE);
  g_assert_cmpuint(b, !=, PT_WS_ID_NONE);
  g_assert_cmpuint(a, !=, b);
  g_assert_cmpuint(pt_workspace_project_count(ws), ==, 2);
  g_assert_cmpuint(pt_workspace_project_at(ws, 0), ==, a);
  g_assert_cmpuint(pt_workspace_project_at(ws, 1), ==, b);
  g_assert_cmpuint(pt_workspace_project_at(ws, 2), ==, PT_WS_ID_NONE);
  g_assert_cmpuint(pt_workspace_project_index(ws, a), ==, 0);
  g_assert_cmpuint(pt_workspace_project_index(ws, b), ==, 1);
  g_assert_cmpstr(pt_workspace_project_name(ws, a), ==, "alpha");
  g_assert_cmpstr(pt_workspace_project_path(ws, b), ==, "/tmp/b");
  /* The first project of an empty workspace becomes active; later adds do
   * not steal it (the window activates the added project explicitly). */
  g_assert_cmpuint(pt_workspace_active_project(ws), ==, a);
  pt_workspace_free(ws);
}

/* -1 takes the next accent in the fixed cycle, decided by the project count at
 * the moment of the add — the same rule as the window's project_ui_alloc,
 * which read projects->len just before the append. */
static void test_accent_cycle(void) {
  PtWorkspace *ws = pt_workspace_new();
  PtWsId ids[PT_ACCENT_COUNT + 2];
  for (guint i = 0; i < PT_ACCENT_COUNT + 2; i++)
    ids[i] = pt_workspace_add_project(ws, "p", "/tmp", -1);
  for (guint i = 0; i < PT_ACCENT_COUNT + 2; i++)
    g_assert_cmpint(pt_workspace_project_accent(ws, ids[i]), ==,
                    (int)(i % PT_ACCENT_COUNT));
  /* After a removal the cycle follows the *current* count, exactly as the
   * window did — not the number ever created. */
  pt_workspace_remove_project(ws, ids[2]);
  PtWsId next = pt_workspace_add_project(ws, "q", "/tmp", -1);
  g_assert_cmpint(pt_workspace_project_accent(ws, next), ==,
                  (int)((PT_ACCENT_COUNT + 1) % PT_ACCENT_COUNT));
  pt_workspace_free(ws);
}

static void test_accent_explicit(void) {
  PtWorkspace *ws = pt_workspace_new();
  /* Explicit accents are stored as given — including 0, which must not fall
   * back to the cycle (the session file distinguishes them). */
  PtWsId a = pt_workspace_add_project(ws, "a", "/tmp", 4);
  PtWsId b = pt_workspace_add_project(ws, "b", "/tmp", 0);
  g_assert_cmpint(pt_workspace_project_accent(ws, a), ==, 4);
  g_assert_cmpint(pt_workspace_project_accent(ws, b), ==, 0);
  g_assert_cmpint(pt_workspace_project_accent(ws, 9999), ==, 0);  /* dead id */
  pt_workspace_free(ws);
}

/* ---------- move ---------- */

/* Port of test-session's index-after-move spot checks, restated for ids: after
 * a move every id still names its project, at the index the old remap
 * computed. */
static void test_move_spot_checks(void) {
  PtWorkspace *ws = pt_workspace_new();
  PtWsId ids[4];
  for (guint i = 0; i < 4; i++)
    ids[i] = pt_workspace_add_project(ws, "p", "/tmp", -1);
  pt_workspace_set_active_project(ws, ids[2]);

  /* Drag project 0 to the end: the active one (old index 2) slides up. */
  pt_workspace_move_project(ws, ids[0], 3);
  g_assert_cmpuint(pt_workspace_project_index(ws, ids[0]), ==, 3);
  g_assert_cmpuint(pt_workspace_project_index(ws, ids[2]), ==, 1);
  g_assert_cmpuint(pt_workspace_project_index(ws, ids[3]), ==, 2);
  g_assert_cmpuint(pt_workspace_active_project(ws), ==, ids[2]);

  /* Drag it back: everything returns home, active identity untouched. */
  pt_workspace_move_project(ws, ids[0], 0);
  for (guint i = 0; i < 4; i++)
    g_assert_cmpuint(pt_workspace_project_index(ws, ids[i]), ==, i);
  g_assert_cmpuint(pt_workspace_active_project(ws), ==, ids[2]);

  /* Dropped on itself: nothing moves. */
  pt_workspace_move_project(ws, ids[2], 2);
  for (guint i = 0; i < 4; i++)
    g_assert_cmpuint(pt_workspace_project_index(ws, ids[i]), ==, i);

  /* Past the end clamps to the end; a dead id is a no-op. */
  pt_workspace_move_project(ws, ids[1], 99);
  g_assert_cmpuint(pt_workspace_project_index(ws, ids[1]), ==, 3);
  pt_workspace_move_project(ws, 9999, 0);
  g_assert_cmpuint(pt_workspace_project_count(ws), ==, 4);
  pt_workspace_free(ws);
}

/* Port of test-session's index-after-move-matches-array: for every (from, to,
 * active) the workspace order must match a plain array steal+insert, and the
 * active project must keep its identity. */
static void test_move_matches_array(void) {
  const guint n = 6;
  for (guint from = 0; from < n; from++) {
    for (guint to = 0; to < n; to++) {
      for (guint act = 0; act < n; act++) {
        PtWorkspace *ws = pt_workspace_new();
        GPtrArray *ref = g_ptr_array_new();
        PtWsId ids[6];
        for (guint i = 0; i < n; i++) {
          ids[i] = pt_workspace_add_project(ws, "p", "/tmp", -1);
          g_ptr_array_add(ref, GUINT_TO_POINTER(ids[i]));
        }
        pt_workspace_set_active_project(ws, ids[act]);
        gpointer moved = g_ptr_array_steal_index(ref, from);
        g_ptr_array_insert(ref, (gint)to, moved);
        pt_workspace_move_project(ws, ids[from], to);
        for (guint i = 0; i < n; i++) {
          PtWsId want = GPOINTER_TO_UINT(g_ptr_array_index(ref, i));
          g_assert_cmpuint(pt_workspace_project_at(ws, i), ==, want);
          g_assert_cmpuint(pt_workspace_project_index(ws, want), ==, i);
        }
        g_assert_cmpuint(pt_workspace_active_project(ws), ==, ids[act]);
        g_ptr_array_free(ref, TRUE);
        pt_workspace_free(ws);
      }
    }
  }
}

/* ---------- project removal ---------- */

static void test_remove_active_project_selects_successor(void) {
  PtWorkspace *ws = pt_workspace_new();
  PtWsId a = pt_workspace_add_project(ws, "a", "/tmp", -1);
  PtWsId b = pt_workspace_add_project(ws, "b", "/tmp", -1);
  PtWsId c = pt_workspace_add_project(ws, "c", "/tmp", -1);
  /* Active in the middle: the successor slides into its slot and is taken. */
  pt_workspace_set_active_project(ws, b);
  pt_workspace_remove_project(ws, b);
  g_assert_cmpuint(pt_workspace_active_project(ws), ==, c);
  g_assert_cmpuint(pt_workspace_project_count(ws), ==, 2);
  g_assert_cmpuint(pt_workspace_project_index(ws, b), ==, PT_WS_INDEX_NONE);
  /* Active at the end: falls back to the new last. */
  pt_workspace_remove_project(ws, c);
  g_assert_cmpuint(pt_workspace_active_project(ws), ==, a);
  /* Last one out: no active project at all. */
  pt_workspace_remove_project(ws, a);
  g_assert_cmpuint(pt_workspace_active_project(ws), ==, PT_WS_ID_NONE);
  g_assert_cmpuint(pt_workspace_project_count(ws), ==, 0);
  pt_workspace_free(ws);
}

static void test_remove_other_project_keeps_active(void) {
  PtWorkspace *ws = pt_workspace_new();
  PtWsId a = pt_workspace_add_project(ws, "a", "/tmp", -1);
  PtWsId b = pt_workspace_add_project(ws, "b", "/tmp", -1);
  PtWsId c = pt_workspace_add_project(ws, "c", "/tmp", -1);
  pt_workspace_set_active_project(ws, b);
  /* Removing before the active one must not switch projects — identity, not
   * position. (The old index bookkeeping got exactly this case wrong.) */
  pt_workspace_remove_project(ws, a);
  g_assert_cmpuint(pt_workspace_active_project(ws), ==, b);
  g_assert_cmpuint(pt_workspace_project_index(ws, b), ==, 0);
  pt_workspace_remove_project(ws, c);
  g_assert_cmpuint(pt_workspace_active_project(ws), ==, b);
  /* Dead id: workspace untouched. */
  pt_workspace_remove_project(ws, a);
  g_assert_cmpuint(pt_workspace_project_count(ws), ==, 1);
  g_assert_cmpuint(pt_workspace_active_project(ws), ==, b);
  pt_workspace_free(ws);
}

static void test_remove_project_drops_tabs(void) {
  PtWorkspace *ws = pt_workspace_new();
  PtWsId p = pt_workspace_add_project(ws, "p", "/tmp", -1);
  PtWsId t1 = pt_workspace_add_tab(ws, p);
  PtWsId t2 = pt_workspace_add_tab(ws, p);
  pt_workspace_set_data(ws, t1, (gpointer)"x");
  pt_workspace_remove_project(ws, p);
  g_assert_cmpuint(pt_workspace_tab_index(ws, t1), ==, PT_WS_INDEX_NONE);
  g_assert_cmpuint(pt_workspace_tab_project(ws, t2), ==, PT_WS_ID_NONE);
  g_assert_cmpuint(pt_workspace_active_tab(ws, p), ==, PT_WS_ID_NONE);
  g_assert_null(pt_workspace_get_data(ws, t1));
  pt_workspace_free(ws);
}

/* ---------- tabs ---------- */

static void test_tab_basics(void) {
  PtWorkspace *ws = pt_workspace_new();
  PtWsId p1 = pt_workspace_add_project(ws, "p1", "/tmp", -1);
  PtWsId p2 = pt_workspace_add_project(ws, "p2", "/tmp", -1);
  PtWsId a = pt_workspace_add_tab(ws, p1);
  PtWsId b = pt_workspace_add_tab(ws, p1);
  PtWsId c = pt_workspace_add_tab(ws, p2);
  g_assert_cmpuint(a, !=, PT_WS_ID_NONE);
  g_assert_cmpuint(pt_workspace_tab_count(ws, p1), ==, 2);
  g_assert_cmpuint(pt_workspace_tab_count(ws, p2), ==, 1);
  g_assert_cmpuint(pt_workspace_tab_at(ws, p1, 0), ==, a);
  g_assert_cmpuint(pt_workspace_tab_at(ws, p1, 1), ==, b);
  g_assert_cmpuint(pt_workspace_tab_at(ws, p1, 2), ==, PT_WS_ID_NONE);
  g_assert_cmpuint(pt_workspace_tab_index(ws, b), ==, 1);
  g_assert_cmpuint(pt_workspace_tab_project(ws, b), ==, p1);
  g_assert_cmpuint(pt_workspace_tab_project(ws, c), ==, p2);
  /* A project's first tab becomes its active tab; later ones do not. */
  g_assert_cmpuint(pt_workspace_active_tab(ws, p1), ==, a);
  g_assert_cmpuint(pt_workspace_active_tab(ws, p2), ==, c);
  /* Tabs of a dead project cannot be added. */
  g_assert_cmpuint(pt_workspace_add_tab(ws, 9999), ==, PT_WS_ID_NONE);
  /* Project queries reject tab ids and vice versa. */
  g_assert_cmpuint(pt_workspace_project_index(ws, a), ==, PT_WS_INDEX_NONE);
  g_assert_cmpuint(pt_workspace_tab_index(ws, p1), ==, PT_WS_INDEX_NONE);
  pt_workspace_free(ws);
}

/* The remove_tab_at rules from the window, id-shaped: removing before the
 * active tab keeps its identity (the old code decremented the index); removing
 * the active one takes the successor, or the new last when it was last. */
static void test_remove_tab_clamping(void) {
  PtWorkspace *ws = pt_workspace_new();
  PtWsId p = pt_workspace_add_project(ws, "p", "/tmp", -1);
  PtWsId a = pt_workspace_add_tab(ws, p);
  PtWsId b = pt_workspace_add_tab(ws, p);
  PtWsId c = pt_workspace_add_tab(ws, p);
  PtWsId d = pt_workspace_add_tab(ws, p);

  /* Remove before the active: identity kept. */
  pt_workspace_set_active_tab(ws, b);
  pt_workspace_remove_tab(ws, a);
  g_assert_cmpuint(pt_workspace_active_tab(ws, p), ==, b);
  g_assert_cmpuint(pt_workspace_tab_index(ws, b), ==, 0);

  /* Remove after the active: identity kept. */
  pt_workspace_remove_tab(ws, d);
  g_assert_cmpuint(pt_workspace_active_tab(ws, p), ==, b);

  /* Remove the active in the middle: the successor slides in and is taken. */
  pt_workspace_remove_tab(ws, b);
  g_assert_cmpuint(pt_workspace_active_tab(ws, p), ==, c);

  /* Remove the active at the end: fall back to the new last. */
  PtWsId e = pt_workspace_add_tab(ws, p);
  pt_workspace_set_active_tab(ws, e);
  pt_workspace_remove_tab(ws, e);
  g_assert_cmpuint(pt_workspace_active_tab(ws, p), ==, c);

  /* Last tab out: no active tab. Dead id: no-op. */
  pt_workspace_remove_tab(ws, c);
  g_assert_cmpuint(pt_workspace_active_tab(ws, p), ==, PT_WS_ID_NONE);
  g_assert_cmpuint(pt_workspace_tab_count(ws, p), ==, 0);
  pt_workspace_remove_tab(ws, c);
  g_assert_cmpuint(pt_workspace_tab_count(ws, p), ==, 0);
  pt_workspace_free(ws);
}

static void test_active_tab_is_per_project(void) {
  PtWorkspace *ws = pt_workspace_new();
  PtWsId p1 = pt_workspace_add_project(ws, "p1", "/tmp", -1);
  PtWsId p2 = pt_workspace_add_project(ws, "p2", "/tmp", -1);
  PtWsId a = pt_workspace_add_tab(ws, p1);
  PtWsId b = pt_workspace_add_tab(ws, p1);
  PtWsId c = pt_workspace_add_tab(ws, p2);
  (void)a; (void)c;
  /* Selecting a background project's tab neither switches projects nor
   * disturbs any other project's selection. */
  pt_workspace_set_active_project(ws, p2);
  pt_workspace_set_active_tab(ws, b);
  g_assert_cmpuint(pt_workspace_active_project(ws), ==, p2);
  g_assert_cmpuint(pt_workspace_active_tab(ws, p1), ==, b);
  g_assert_cmpuint(pt_workspace_active_tab(ws, p2), ==, c);
  pt_workspace_free(ws);
}

static void test_set_active_rejects_wrong_ids(void) {
  PtWorkspace *ws = pt_workspace_new();
  PtWsId p = pt_workspace_add_project(ws, "p", "/tmp", -1);
  PtWsId t = pt_workspace_add_tab(ws, p);
  PtWsId q = pt_workspace_add_project(ws, "q", "/tmp", -1);
  pt_workspace_set_active_project(ws, q);
  /* Wrong kind and dead ids leave the selection alone. */
  pt_workspace_set_active_project(ws, t);
  g_assert_cmpuint(pt_workspace_active_project(ws), ==, q);
  pt_workspace_set_active_project(ws, 9999);
  g_assert_cmpuint(pt_workspace_active_project(ws), ==, q);
  pt_workspace_set_active_tab(ws, p);
  g_assert_cmpuint(pt_workspace_active_tab(ws, p), ==, t);
  pt_workspace_set_active_tab(ws, 9999);
  g_assert_cmpuint(pt_workspace_active_tab(ws, p), ==, t);
  pt_workspace_free(ws);
}

/* ---------- data slots and id lifetime ---------- */

static void test_data_slots(void) {
  PtWorkspace *ws = pt_workspace_new();
  PtWsId p = pt_workspace_add_project(ws, "p", "/tmp", -1);
  PtWsId q = pt_workspace_add_project(ws, "q", "/tmp", -1);
  PtWsId t = pt_workspace_add_tab(ws, p);
  int pd = 1, td = 2;
  pt_workspace_set_data(ws, p, &pd);
  pt_workspace_set_data(ws, t, &td);
  g_assert_true(pt_workspace_get_data(ws, p) == &pd);
  g_assert_true(pt_workspace_get_data(ws, t) == &td);
  g_assert_null(pt_workspace_get_data(ws, q));
  /* Slots ride the id, not the position. */
  pt_workspace_move_project(ws, p, 1);
  g_assert_true(pt_workspace_get_data(ws, p) == &pd);
  /* A dead id's slot is gone; setting one is a no-op, not a resurrection. */
  pt_workspace_remove_tab(ws, t);
  g_assert_null(pt_workspace_get_data(ws, t));
  pt_workspace_set_data(ws, t, &td);
  g_assert_null(pt_workspace_get_data(ws, t));
  pt_workspace_free(ws);
}

static void test_ids_never_reused(void) {
  PtWorkspace *ws = pt_workspace_new();
  GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
  for (int round = 0; round < 5; round++) {
    PtWsId p = pt_workspace_add_project(ws, "p", "/tmp", -1);
    PtWsId t1 = pt_workspace_add_tab(ws, p);
    PtWsId t2 = pt_workspace_add_tab(ws, p);
    g_assert_true(g_hash_table_add(seen, GUINT_TO_POINTER(p)));
    g_assert_true(g_hash_table_add(seen, GUINT_TO_POINTER(t1)));
    g_assert_true(g_hash_table_add(seen, GUINT_TO_POINTER(t2)));
    pt_workspace_remove_tab(ws, t1);
    pt_workspace_remove_project(ws, p);
  }
  g_hash_table_unref(seen);
  pt_workspace_free(ws);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/workspace/empty", test_empty);
  g_test_add_func("/workspace/add-project-basics", test_add_project_basics);
  g_test_add_func("/workspace/accent-cycle", test_accent_cycle);
  g_test_add_func("/workspace/accent-explicit", test_accent_explicit);
  g_test_add_func("/workspace/move-spot-checks", test_move_spot_checks);
  g_test_add_func("/workspace/move-matches-array", test_move_matches_array);
  g_test_add_func("/workspace/remove-active-project-selects-successor",
                  test_remove_active_project_selects_successor);
  g_test_add_func("/workspace/remove-other-project-keeps-active",
                  test_remove_other_project_keeps_active);
  g_test_add_func("/workspace/remove-project-drops-tabs",
                  test_remove_project_drops_tabs);
  g_test_add_func("/workspace/tab-basics", test_tab_basics);
  g_test_add_func("/workspace/remove-tab-clamping", test_remove_tab_clamping);
  g_test_add_func("/workspace/active-tab-is-per-project",
                  test_active_tab_is_per_project);
  g_test_add_func("/workspace/set-active-rejects-wrong-ids",
                  test_set_active_rejects_wrong_ids);
  g_test_add_func("/workspace/data-slots", test_data_slots);
  g_test_add_func("/workspace/ids-never-reused", test_ids_never_reused);
  return g_test_run();
}
