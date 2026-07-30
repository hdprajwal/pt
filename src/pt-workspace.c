#include "pt-workspace.h"

#include "pt-session.h"   /* PT_ACCENT_COUNT */

/* Two id namespaces would invite passing the wrong kind, so there is one:
 * every id comes off the same counter, and each lookup table says what kind
 * its ids are. Entries live in the ordered arrays; the hash tables borrow. */

typedef struct WsProject WsProject;

typedef struct {
  PtWsId id;
  WsProject *owner;
  gpointer data;
} WsTab;

struct WsProject {
  PtWsId id;
  char *name;
  char *path;
  int accent;
  GPtrArray *tabs;      /* WsTab*, ordered, owned (ws_tab_free) */
  PtWsId active_tab;    /* PT_WS_ID_NONE when the project has no tabs */
  gpointer data;
};

struct PtWorkspace {
  GPtrArray *projects;        /* WsProject*, ordered, owned */
  GHashTable *projects_by_id; /* GUINT_TO_POINTER(id) → WsProject*, borrowed */
  GHashTable *tabs_by_id;     /* GUINT_TO_POINTER(id) → WsTab*, borrowed */
  PtWsId active_project;      /* PT_WS_ID_NONE when empty */
  guint32 next_id;            /* monotonically increasing; ids never reused */
};

/* ---------- internal lookups ---------- */

static WsProject *project_ref(const PtWorkspace *ws, PtWsId id) {
  return g_hash_table_lookup(ws->projects_by_id, GUINT_TO_POINTER(id));
}

static WsTab *tab_ref(const PtWorkspace *ws, PtWsId id) {
  return g_hash_table_lookup(ws->tabs_by_id, GUINT_TO_POINTER(id));
}

/* ---------- lifecycle ---------- */

static void ws_tab_free(gpointer data) {
  g_free(data);
}

static void ws_project_free(gpointer data) {
  WsProject *p = data;
  g_free(p->name);
  g_free(p->path);
  g_ptr_array_free(p->tabs, TRUE);
  g_free(p);
}

PtWorkspace *pt_workspace_new(void) {
  PtWorkspace *ws = g_new0(PtWorkspace, 1);
  ws->projects = g_ptr_array_new_with_free_func(ws_project_free);
  ws->projects_by_id = g_hash_table_new(g_direct_hash, g_direct_equal);
  ws->tabs_by_id = g_hash_table_new(g_direct_hash, g_direct_equal);
  ws->next_id = 1;   /* 0 is PT_WS_ID_NONE, forever */
  return ws;
}

void pt_workspace_free(PtWorkspace *ws) {
  if (ws == NULL) return;
  g_ptr_array_free(ws->projects, TRUE);
  g_hash_table_unref(ws->projects_by_id);
  g_hash_table_unref(ws->tabs_by_id);
  g_free(ws);
}

/* ---------- projects ---------- */

PtWsId pt_workspace_add_project(PtWorkspace *ws, const char *name,
                                const char *path, int accent) {
  WsProject *p = g_new0(WsProject, 1);
  p->id = ws->next_id++;
  p->name = g_strdup(name);
  p->path = g_strdup(path);
  /* -1: next in the cycle, by the project's position-to-be — the array length
   * right now, before the append. */
  p->accent = accent >= 0 ? accent
                          : (int)(ws->projects->len % PT_ACCENT_COUNT);
  p->tabs = g_ptr_array_new_with_free_func(ws_tab_free);
  p->active_tab = PT_WS_ID_NONE;
  g_ptr_array_add(ws->projects, p);
  g_hash_table_insert(ws->projects_by_id, GUINT_TO_POINTER(p->id), p);
  /* The first project of an empty workspace is active by construction; every
   * later add leaves the selection to the caller. */
  if (ws->active_project == PT_WS_ID_NONE)
    ws->active_project = p->id;
  return p->id;
}

/* Shared by both removals: with `victim` about to leave `arr` (an array of
 * structs whose first member is the id), pick what `active` becomes. The
 * successor slides into the victim's slot and is taken; when the victim was
 * last, the new last; when it was alone, none. A non-active victim changes
 * nothing — identity, not position. */
static PtWsId active_after_remove(GPtrArray *arr, guint victim_index,
                                  PtWsId victim_id, PtWsId active) {
  if (active != victim_id) return active;
  if (arr->len <= 1) return PT_WS_ID_NONE;
  guint next = victim_index + 1 < arr->len ? victim_index + 1
                                           : victim_index - 1;
  /* Both WsProject and WsTab start with their id. */
  return *(PtWsId *)g_ptr_array_index(arr, next);
}

void pt_workspace_remove_project(PtWorkspace *ws, PtWsId project) {
  WsProject *p = project_ref(ws, project);
  if (p == NULL) return;
  guint index = 0;
  if (!g_ptr_array_find(ws->projects, p, &index)) return;   /* can't happen */
  ws->active_project =
      active_after_remove(ws->projects, index, p->id, ws->active_project);
  for (guint i = 0; i < p->tabs->len; i++) {
    WsTab *t = g_ptr_array_index(p->tabs, i);
    g_hash_table_remove(ws->tabs_by_id, GUINT_TO_POINTER(t->id));
  }
  g_hash_table_remove(ws->projects_by_id, GUINT_TO_POINTER(p->id));
  g_ptr_array_remove_index(ws->projects, index);   /* frees p and its tabs */
}

void pt_workspace_move_project(PtWorkspace *ws, PtWsId project,
                               guint new_index) {
  WsProject *p = project_ref(ws, project);
  if (p == NULL) return;
  guint from = 0;
  if (!g_ptr_array_find(ws->projects, p, &from)) return;
  guint to = MIN(new_index, ws->projects->len - 1);
  if (from == to) return;
  g_ptr_array_steal_index(ws->projects, from);
  g_ptr_array_insert(ws->projects, (gint)to, p);
  /* Nothing else moves: every id, active ones included, keeps its meaning. */
}

/* ---------- tabs ---------- */

PtWsId pt_workspace_add_tab(PtWorkspace *ws, PtWsId project) {
  WsProject *p = project_ref(ws, project);
  if (p == NULL) return PT_WS_ID_NONE;
  WsTab *t = g_new0(WsTab, 1);
  t->id = ws->next_id++;
  t->owner = p;
  g_ptr_array_add(p->tabs, t);
  g_hash_table_insert(ws->tabs_by_id, GUINT_TO_POINTER(t->id), t);
  /* Same rule as add_project: a project whose only tab this is shows it. */
  if (p->active_tab == PT_WS_ID_NONE)
    p->active_tab = t->id;
  return t->id;
}

void pt_workspace_remove_tab(PtWorkspace *ws, PtWsId tab) {
  WsTab *t = tab_ref(ws, tab);
  if (t == NULL) return;
  WsProject *p = t->owner;
  guint index = 0;
  if (!g_ptr_array_find(p->tabs, t, &index)) return;   /* can't happen */
  p->active_tab = active_after_remove(p->tabs, index, t->id, p->active_tab);
  g_hash_table_remove(ws->tabs_by_id, GUINT_TO_POINTER(t->id));
  g_ptr_array_remove_index(p->tabs, index);   /* frees t */
}

/* ---------- selection ---------- */

void pt_workspace_set_active_project(PtWorkspace *ws, PtWsId project) {
  if (project_ref(ws, project) != NULL)
    ws->active_project = project;
}

void pt_workspace_set_active_tab(PtWorkspace *ws, PtWsId tab) {
  WsTab *t = tab_ref(ws, tab);
  if (t != NULL)
    t->owner->active_tab = tab;
}

PtWsId pt_workspace_active_project(const PtWorkspace *ws) {
  return ws->active_project;
}

PtWsId pt_workspace_active_tab(const PtWorkspace *ws, PtWsId project) {
  WsProject *p = project_ref(ws, project);
  return p != NULL ? p->active_tab : PT_WS_ID_NONE;
}

/* ---------- iteration ---------- */

guint pt_workspace_project_count(const PtWorkspace *ws) {
  return ws->projects->len;
}

PtWsId pt_workspace_project_at(const PtWorkspace *ws, guint index) {
  if (index >= ws->projects->len) return PT_WS_ID_NONE;
  return ((WsProject *)g_ptr_array_index(ws->projects, index))->id;
}

guint pt_workspace_project_index(const PtWorkspace *ws, PtWsId project) {
  WsProject *p = project_ref(ws, project);
  guint index = 0;
  if (p == NULL || !g_ptr_array_find(ws->projects, p, &index))
    return PT_WS_INDEX_NONE;
  return index;
}

guint pt_workspace_tab_count(const PtWorkspace *ws, PtWsId project) {
  WsProject *p = project_ref(ws, project);
  return p != NULL ? p->tabs->len : 0;
}

PtWsId pt_workspace_tab_at(const PtWorkspace *ws, PtWsId project,
                           guint index) {
  WsProject *p = project_ref(ws, project);
  if (p == NULL || index >= p->tabs->len) return PT_WS_ID_NONE;
  return ((WsTab *)g_ptr_array_index(p->tabs, index))->id;
}

guint pt_workspace_tab_index(const PtWorkspace *ws, PtWsId tab) {
  WsTab *t = tab_ref(ws, tab);
  guint index = 0;
  if (t == NULL || !g_ptr_array_find(t->owner->tabs, t, &index))
    return PT_WS_INDEX_NONE;
  return index;
}

PtWsId pt_workspace_tab_project(const PtWorkspace *ws, PtWsId tab) {
  WsTab *t = tab_ref(ws, tab);
  return t != NULL ? t->owner->id : PT_WS_ID_NONE;
}

/* ---------- identity ---------- */

const char *pt_workspace_project_name(const PtWorkspace *ws, PtWsId project) {
  WsProject *p = project_ref(ws, project);
  return p != NULL ? p->name : NULL;
}

const char *pt_workspace_project_path(const PtWorkspace *ws, PtWsId project) {
  WsProject *p = project_ref(ws, project);
  return p != NULL ? p->path : NULL;
}

int pt_workspace_project_accent(const PtWorkspace *ws, PtWsId project) {
  WsProject *p = project_ref(ws, project);
  return p != NULL ? p->accent : 0;
}

/* ---------- data slots ---------- */

void pt_workspace_set_data(PtWorkspace *ws, PtWsId id, gpointer data) {
  WsProject *p = project_ref(ws, id);
  if (p != NULL) { p->data = data; return; }
  WsTab *t = tab_ref(ws, id);
  if (t != NULL) t->data = data;
}

gpointer pt_workspace_get_data(const PtWorkspace *ws, PtWsId id) {
  WsProject *p = project_ref(ws, id);
  if (p != NULL) return p->data;
  WsTab *t = tab_ref(ws, id);
  return t != NULL ? t->data : NULL;
}
