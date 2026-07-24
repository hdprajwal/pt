#include "pt-pane-grid.h"

enum { SIG_STRUCTURE, SIG_ACTIVITY, SIG_FOCUS, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _PtPaneGrid {
  GtkWidget parent_instance;
  PtSplitNode *tree;
  PtSplitNode *focused;   /* always a leaf of tree, or NULL when empty */
  GtkWidget *root_widget; /* current widget tree child */
};

G_DEFINE_FINAL_TYPE(PtPaneGrid, pt_pane_grid, GTK_TYPE_WIDGET)

static void on_term_activity(PtTerminal *t, gpointer user) {
  (void)t;
  g_signal_emit(PT_PANE_GRID(user), signals[SIG_ACTIVITY], 0);
}

static void on_term_focus_enter(GtkEventControllerFocus *ctl, gpointer user) {
  PtPaneGrid *g = PT_PANE_GRID(user);
  GtkWidget *term =
      gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctl));
  PtSplitNode *leaf = g_object_get_data(G_OBJECT(term), "pt-leaf");
  if (leaf != NULL && leaf != g->focused) {
    g->focused = leaf;
    g_signal_emit(g, signals[SIG_FOCUS], 0);
  }
}

static GtkWidget *ensure_terminal(PtPaneGrid *g, PtSplitNode *leaf) {
  if (leaf->user != NULL) return GTK_WIDGET(leaf->user);
  GtkWidget *term = pt_terminal_new(leaf->cwd);
  g_object_ref_sink(term);
  leaf->user = term;
  g_object_set_data(G_OBJECT(term), "pt-leaf", leaf);
  g_signal_connect(term, "activity", G_CALLBACK(on_term_activity), g);
  GtkEventController *focus = gtk_event_controller_focus_new();
  g_signal_connect(focus, "enter", G_CALLBACK(on_term_focus_enter), g);
  gtk_widget_add_controller(term, focus);
  return term;
}

/* Detach every terminal from the old widget tree so paneds can be dropped. */
static void detach_terminals(PtSplitNode *n) {
  if (n == NULL) return;
  if (n->kind == PT_SPLIT_LEAF) {
    GtkWidget *term = n->user;
    if (term != NULL && gtk_widget_get_parent(term) != NULL)
      gtk_widget_unparent(term);
    return;
  }
  detach_terminals(n->a);
  detach_terminals(n->b);
}

static gboolean apply_ratio_idle(gpointer data) {
  GtkPaned *paned = GTK_PANED(data);
  if (gtk_widget_get_parent(GTK_WIDGET(paned)) == NULL) {
    g_object_unref(paned);
    return G_SOURCE_REMOVE;
  }
  PtSplitNode *node = g_object_get_data(G_OBJECT(paned), "pt-node");
  if (node != NULL) {
    int total =
        (node->kind == PT_SPLIT_H)
            ? gtk_widget_get_width(GTK_WIDGET(paned))
            : gtk_widget_get_height(GTK_WIDGET(paned));
    if (total > 0)
      gtk_paned_set_position(paned, (int)(total * node->ratio));
  }
  g_object_unref(paned);
  return G_SOURCE_REMOVE;
}

static void on_paned_position(GObject *obj, GParamSpec *spec, gpointer user) {
  (void)spec; (void)user;
  GtkPaned *paned = GTK_PANED(obj);
  if (gtk_widget_get_parent(GTK_WIDGET(paned)) == NULL) return;
  PtSplitNode *node = g_object_get_data(obj, "pt-node");
  if (node == NULL) return;
  int total = (node->kind == PT_SPLIT_H)
                  ? gtk_widget_get_width(GTK_WIDGET(paned))
                  : gtk_widget_get_height(GTK_WIDGET(paned));
  if (total > 0)
    node->ratio = (double)gtk_paned_get_position(paned) / total;
}

static GtkWidget *build_widgets(PtPaneGrid *g, PtSplitNode *n) {
  if (n->kind == PT_SPLIT_LEAF) return ensure_terminal(g, n);
  GtkWidget *paned = gtk_paned_new(n->kind == PT_SPLIT_H
                                       ? GTK_ORIENTATION_HORIZONTAL
                                       : GTK_ORIENTATION_VERTICAL);
  gtk_paned_set_start_child(GTK_PANED(paned), build_widgets(g, n->a));
  gtk_paned_set_end_child(GTK_PANED(paned), build_widgets(g, n->b));
  gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
  gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
  g_object_set_data(G_OBJECT(paned), "pt-node", n);
  g_signal_connect(paned, "notify::position",
                   G_CALLBACK(on_paned_position), g);
  g_idle_add(apply_ratio_idle, g_object_ref(paned));
  return paned;
}

/* Clear "pt-node" qdata on every paned in the old widget tree so no dangling
 * PtSplitNode* survives teardown (nodes may be freed before an idle fires). */
static void clear_paned_nodes(GtkWidget *w) {
  if (w == NULL) return;
  if (GTK_IS_PANED(w)) g_object_set_data(G_OBJECT(w), "pt-node", NULL);
  for (GtkWidget *c = gtk_widget_get_first_child(w); c != NULL;
       c = gtk_widget_get_next_sibling(c))
    clear_paned_nodes(c);
}

static void rebuild(PtPaneGrid *g) {
  clear_paned_nodes(g->root_widget);
  detach_terminals(g->tree);
  if (g->root_widget != NULL) {
    gtk_widget_unparent(g->root_widget);
    g->root_widget = NULL;
  }
  if (g->tree == NULL) return;
  g->root_widget = build_widgets(g, g->tree);
  gtk_widget_set_parent(g->root_widget, GTK_WIDGET(g));
}

GtkWidget *pt_pane_grid_new(PtSplitNode *tree) {
  PtPaneGrid *g = g_object_new(PT_TYPE_PANE_GRID, NULL);
  g->tree = tree;
  g->focused = pt_split_first_leaf(tree);
  rebuild(g);
  return GTK_WIDGET(g);
}

PtSplitNode *pt_pane_grid_tree(PtPaneGrid *g) { return g->tree; }

void pt_pane_grid_split(PtPaneGrid *g, PtSplitKind kind) {
  if (g->focused == NULL) return;
  /* New pane starts in the focused pane's *current* directory. */
  PtTerminal *ft = PT_TERMINAL(g->focused->user);
  char *cwd = ft != NULL ? pt_terminal_current_cwd(ft) : NULL;
  PtSplitNode *fresh = pt_split_split(&g->tree, g->focused, kind);
  if (cwd != NULL) { g_free(fresh->cwd); fresh->cwd = cwd; }
  rebuild(g);
  g->focused = fresh;
  pt_pane_grid_focus_terminal(g);
  g_signal_emit(g, signals[SIG_STRUCTURE], 0);
  g_signal_emit(g, signals[SIG_FOCUS], 0);
}

gboolean pt_pane_grid_close_focused(PtPaneGrid *g) {
  if (g->focused == NULL) return FALSE;
  GtkWidget *term = g->focused->user;
  if (term != NULL) {
    if (gtk_widget_get_parent(term) != NULL) gtk_widget_unparent(term);
    g_object_unref(term);
    g->focused->user = NULL;
  }
  PtSplitNode *next = pt_split_close(&g->tree, g->focused);
  g->focused = next;
  rebuild(g);
  if (next == NULL) return FALSE;
  pt_pane_grid_focus_terminal(g);
  g_signal_emit(g, signals[SIG_STRUCTURE], 0);
  g_signal_emit(g, signals[SIG_FOCUS], 0);
  return TRUE;
}

void pt_pane_grid_focus_next(PtPaneGrid *g) {
  if (g->focused == NULL) return;
  g->focused = pt_split_next_leaf(g->tree, g->focused);
  pt_pane_grid_focus_terminal(g);
  g_signal_emit(g, signals[SIG_FOCUS], 0);
}

PtTerminal *pt_pane_grid_focused_terminal(PtPaneGrid *g) {
  return g->focused != NULL ? PT_TERMINAL(g->focused->user) : NULL;
}

int pt_pane_grid_pane_count(PtPaneGrid *g) {
  return pt_split_count_leaves(g->tree);
}

static void index_walk(PtSplitNode *n, PtSplitNode *target, int *idx,
                       int *found) {
  if (n == NULL || *found >= 0) return;
  if (n->kind == PT_SPLIT_LEAF) {
    if (n == target) *found = *idx;
    (*idx)++;
    return;
  }
  index_walk(n->a, target, idx, found);
  index_walk(n->b, target, idx, found);
}

int pt_pane_grid_focused_index(PtPaneGrid *g) {
  int idx = 0, found = -1;
  index_walk(g->tree, g->focused, &idx, &found);
  return found >= 0 ? found : 0;
}

static void sync_cwd_walk(PtSplitNode *n) {
  if (n == NULL) return;
  if (n->kind == PT_SPLIT_LEAF) {
    if (n->user != NULL) {
      char *cwd = pt_terminal_current_cwd(PT_TERMINAL(n->user));
      if (cwd != NULL) { g_free(n->cwd); n->cwd = cwd; }
    }
    return;
  }
  sync_cwd_walk(n->a);
  sync_cwd_walk(n->b);
}

void pt_pane_grid_sync_cwds(PtPaneGrid *g) { sync_cwd_walk(g->tree); }

void pt_pane_grid_focus_terminal(PtPaneGrid *g) {
  if (g->focused != NULL && g->focused->user != NULL)
    gtk_widget_grab_focus(GTK_WIDGET(g->focused->user));
}

static void free_terminals(PtSplitNode *n) {
  if (n == NULL) return;
  if (n->kind == PT_SPLIT_LEAF) {
    if (n->user != NULL) {
      GtkWidget *term = n->user;
      if (gtk_widget_get_parent(term) != NULL) gtk_widget_unparent(term);
      g_object_unref(term);
      n->user = NULL;
    }
    return;
  }
  free_terminals(n->a);
  free_terminals(n->b);
}

static void pt_pane_grid_dispose(GObject *obj) {
  PtPaneGrid *g = PT_PANE_GRID(obj);
  if (g->root_widget != NULL) {
    clear_paned_nodes(g->root_widget);
    detach_terminals(g->tree);
    gtk_widget_unparent(g->root_widget);
    g->root_widget = NULL;
  }
  free_terminals(g->tree);
  g_clear_pointer(&g->tree, pt_split_free);
  G_OBJECT_CLASS(pt_pane_grid_parent_class)->dispose(obj);
}

static void pt_pane_grid_class_init(PtPaneGridClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_pane_grid_dispose;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass),
                                           GTK_TYPE_BIN_LAYOUT);
  signals[SIG_STRUCTURE] = g_signal_new("structure-changed", PT_TYPE_PANE_GRID,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_ACTIVITY] = g_signal_new("activity", PT_TYPE_PANE_GRID,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_FOCUS] = g_signal_new("focus-changed", PT_TYPE_PANE_GRID,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void pt_pane_grid_init(PtPaneGrid *g) {
  gtk_widget_set_hexpand(GTK_WIDGET(g), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(g), TRUE);
}
