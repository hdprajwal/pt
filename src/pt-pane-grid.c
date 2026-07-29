#include "pt-pane-grid.h"

enum { SIG_STRUCTURE, SIG_FOCUS, SIG_COMMAND, SIG_TITLE,
       SIG_EMPTIED, SIG_NOTIFICATION, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _PtPaneGrid {
  GtkWidget parent_instance;
  PtSplitNode *tree;
  PtSplitNode *focused;   /* always a leaf of tree, or NULL when empty */
  GtkWidget *root_widget; /* current widget tree child */
  char **env;             /* child env for the panes we build, or NULL */
};

G_DEFINE_FINAL_TYPE(PtPaneGrid, pt_pane_grid, GTK_TYPE_WIDGET)

/* Re-emit "command-changed" for the currently focused pane, when known, so the
 * tab relabels on focus moves (not just when the fg program itself changes). */
static void emit_focused_command(PtPaneGrid *g) {
  if (g->focused == NULL || g->focused->user == NULL) return;
  const char *cmd = pt_terminal_last_command(PT_TERMINAL(g->focused->user));
  if (cmd != NULL) g_signal_emit(g, signals[SIG_COMMAND], 0, cmd);
}

static void on_term_focus_enter(GtkEventControllerFocus *ctl, gpointer user) {
  PtPaneGrid *g = PT_PANE_GRID(user);
  GtkWidget *term =
      gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctl));
  PtSplitNode *leaf = g_object_get_data(G_OBJECT(term), "pt-leaf");
  if (leaf != NULL && leaf != g->focused) {
    g->focused = leaf;
    g_signal_emit(g, signals[SIG_FOCUS], 0);
    emit_focused_command(g);
  }
}

static void on_term_command(PtTerminal *t, const char *comm, gpointer user) {
  PtPaneGrid *g = PT_PANE_GRID(user);
  if (g->focused != NULL && g->focused->user == (gpointer)t)
    g_signal_emit(g, signals[SIG_COMMAND], 0, comm);
}

/* The prompt reports the previous command's exit code through the terminal
 * title, which lands the instant the prompt is redrawn — well before the 700ms
 * comm poll notices the foreground program went back to the shell. Re-emitting
 * it lets the window repaint "✗ exit 1" (and the tab dot) with no lag. */
static void on_term_title(PtTerminal *t, const char *title, gpointer user) {
  PtPaneGrid *g = PT_PANE_GRID(user);
  if (g->focused != NULL && g->focused->user == (gpointer)t)
    g_signal_emit(g, signals[SIG_TITLE], 0, title);
}

/* Unlike command-changed and title-changed above, this is forwarded from every
 * pane and not just the focused one — the whole point of a desktop
 * notification is that it comes from a pane the user is not watching. The
 * pane's id rides along because the notification has to be clickable back to
 * the pane that raised it, minutes later and possibly from another tab. */
static void on_term_notification(PtTerminal *t, const char *title,
                                 const char *body, gpointer user) {
  g_signal_emit(PT_PANE_GRID(user), signals[SIG_NOTIFICATION], 0,
                pt_terminal_id(t), title, body);
}

/* Is `leaf` still a live leaf of `n`? Compares by pointer identity only, so a
 * dangling `leaf` (freed by a manual close before the idle ran) is never
 * dereferenced — we only deref the tree nodes we walk. */
static gboolean leaf_in_tree(PtSplitNode *n, PtSplitNode *leaf) {
  if (n == NULL) return FALSE;
  if (n->kind == PT_SPLIT_LEAF) return n == leaf;
  return leaf_in_tree(n->a, leaf) || leaf_in_tree(n->b, leaf);
}

typedef struct {
  PtPaneGrid *grid; /* owns a ref */
  PtSplitNode *leaf;
} CloseCtx;

/* Deferred close for a clean shell exit: running synchronously from the
 * "exited" signal would free the terminal core under its own child-watch
 * callback frame. */
static gboolean close_pane_idle(gpointer data) {
  CloseCtx *ctx = data;
  PtPaneGrid *g = ctx->grid;
  if (leaf_in_tree(g->tree, ctx->leaf)) {
    g->focused = ctx->leaf;
    if (!pt_pane_grid_close_focused(g)) /* grid now empty */
      g_signal_emit(g, signals[SIG_EMPTIED], 0);
  }
  g_object_unref(g);
  g_free(ctx);
  return G_SOURCE_REMOVE;
}

static void on_term_exited(PtTerminal *t, int status, gpointer user) {
  if (status != 0) return; /* non-zero/signal exits keep the banner */
  PtPaneGrid *g = PT_PANE_GRID(user);
  PtSplitNode *leaf = g_object_get_data(G_OBJECT(t), "pt-leaf");
  if (leaf == NULL) return;
  CloseCtx *ctx = g_new0(CloseCtx, 1);
  ctx->grid = g_object_ref(g);
  ctx->leaf = leaf;
  g_idle_add(close_pane_idle, ctx);
}

static GtkWidget *ensure_terminal(PtPaneGrid *g, PtSplitNode *leaf) {
  if (leaf->user != NULL) return GTK_WIDGET(leaf->user);
  GtkWidget *term = pt_terminal_new(leaf->cwd);
  /* Before the pane is parented, and so before it can allocate and spawn. */
  pt_terminal_set_spawn_env(PT_TERMINAL(term),
                            (const char *const *)g->env);
  g_object_ref_sink(term);
  leaf->user = term;
  g_object_set_data(G_OBJECT(term), "pt-leaf", leaf);
  g_signal_connect(term, "exited", G_CALLBACK(on_term_exited), g);
  g_signal_connect(term, "command-changed", G_CALLBACK(on_term_command), g);
  g_signal_connect(term, "title-changed", G_CALLBACK(on_term_title), g);
  g_signal_connect(term, "notification", G_CALLBACK(on_term_notification), g);
  GtkEventController *focus = gtk_event_controller_focus_new();
  g_signal_connect(focus, "enter", G_CALLBACK(on_term_focus_enter), g);
  gtk_widget_add_controller(term, focus);
  return term;
}

/* Detach a widget through its parent's own child-management API. A GtkPaned
 * tracks its start/end children in dedicated pointers; a bare
 * gtk_widget_unparent() leaves those stale, and when the old paned is later
 * disposed (event dispatch can keep it alive past a rebuild) its dispose
 * unparents the stale pointers — ripping the widget out of whatever NEW
 * parent it has by then. Blank panes. Mirrors ghostty's gtk detachWidget. */
static void detach_from_parent(GtkWidget *w) {
  GtkWidget *parent = gtk_widget_get_parent(w);
  if (parent == NULL) return;
  if (GTK_IS_PANED(parent)) {
    GtkPaned *p = GTK_PANED(parent);
    if (gtk_paned_get_start_child(p) == w) {
      gtk_paned_set_start_child(p, NULL);
      return;
    }
    if (gtk_paned_get_end_child(p) == w) {
      gtk_paned_set_end_child(p, NULL);
      return;
    }
  }
  gtk_widget_unparent(w);
}

/* Detach every terminal from the old widget tree so paneds can be dropped. */
static void detach_terminals(PtSplitNode *n) {
  if (n == NULL) return;
  if (n->kind == PT_SPLIT_LEAF) {
    if (n->user != NULL) detach_from_parent(n->user);
    return;
  }
  detach_terminals(n->a);
  detach_terminals(n->b);
}

/* Restore node->ratio on a fresh paned. A tick callback, not an idle: the
 * paned has no size until its first allocation (an idle can fire before that
 * and would have to give up), and a hidden tab's paned only allocates when
 * the tab is shown — the tick just waits until a frame gives it a size. */
static gboolean apply_ratio_tick(GtkWidget *w, GdkFrameClock *clock,
                                 gpointer data) {
  (void)clock; (void)data;
  PtSplitNode *node = g_object_get_data(G_OBJECT(w), "pt-node");
  if (node == NULL) return G_SOURCE_REMOVE; /* torn down before it sized */
  int total = (node->kind == PT_SPLIT_H) ? gtk_widget_get_width(w)
                                         : gtk_widget_get_height(w);
  if (total <= 0) return G_SOURCE_CONTINUE; /* not allocated yet */
  gtk_paned_set_position(GTK_PANED(w), (int)(total * node->ratio));
  /* Armed only now: notify::position fired by GTK's own allocation pass
   * (natural-size split) must not write back into node->ratio, or every
   * rebuild resets the layout to GTK's ~50% guess before this restore runs. */
  g_object_set_data(G_OBJECT(w), "pt-ratio-applied", GINT_TO_POINTER(1));
  return G_SOURCE_REMOVE;
}

static void on_paned_position(GObject *obj, GParamSpec *spec, gpointer user) {
  (void)spec; (void)user;
  GtkPaned *paned = GTK_PANED(obj);
  if (gtk_widget_get_parent(GTK_WIDGET(paned)) == NULL) return;
  PtSplitNode *node = g_object_get_data(obj, "pt-node");
  if (node == NULL) return;
  if (g_object_get_data(obj, "pt-ratio-applied") == NULL) return;
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
  gtk_widget_add_tick_callback(paned, apply_ratio_tick, NULL, NULL);
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

static void set_env_walk(PtSplitNode *n, char **envv) {
  if (n == NULL) return;
  if (n->kind == PT_SPLIT_LEAF) {
    if (n->user != NULL)
      pt_terminal_set_spawn_env(PT_TERMINAL(n->user),
                                (const char *const *)envv);
    return;
  }
  set_env_walk(n->a, envv);
  set_env_walk(n->b, envv);
}

void pt_pane_grid_set_env(PtPaneGrid *g, const char *const *envv) {
  g_clear_pointer(&g->env, g_strfreev);
  if (envv != NULL) g->env = g_strdupv((char **)envv);
  /* The panes that already exist get it too: a pane whose shell has not
   * spawned yet (a restored tab never shown, so never allocated) must not be
   * left with the env of whatever the grid was told first. */
  set_env_walk(g->tree, g->env);
}

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
    detach_from_parent(term);
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
  emit_focused_command(g);
}

void pt_pane_grid_focus_prev(PtPaneGrid *g) {
  if (g->focused == NULL) return;
  PtSplitNode *leaf = pt_split_prev_leaf(g->tree, g->focused);
  if (leaf == g->focused) return;
  g->focused = leaf;
  pt_pane_grid_focus_terminal(g);
  g_signal_emit(g, signals[SIG_FOCUS], 0);
  emit_focused_command(g);
}

/* Center of a leaf's terminal in grid coordinates; FALSE if not computable. */
static gboolean leaf_center(PtPaneGrid *g, PtSplitNode *leaf,
                            double *cx, double *cy) {
  if (leaf == NULL || leaf->user == NULL) return FALSE;
  graphene_rect_t b;
  if (!gtk_widget_compute_bounds(GTK_WIDGET(leaf->user), GTK_WIDGET(g), &b))
    return FALSE;
  *cx = b.origin.x + b.size.width / 2.0;
  *cy = b.origin.y + b.size.height / 2.0;
  return TRUE;
}

void pt_pane_grid_focus_direction(PtPaneGrid *g, PtPaneDirection dir) {
  if (g->focused == NULL || g->tree == NULL) return;
  double cx, cy;
  if (!leaf_center(g, g->focused, &cx, &cy)) return;

  PtSplitNode *first = pt_split_first_leaf(g->tree);
  PtSplitNode *best = NULL;
  double best_score = 0;
  PtSplitNode *leaf = first;
  do {
    if (leaf != g->focused) {
      double lx, ly;
      if (leaf_center(g, leaf, &lx, &ly)) {
        /* Distance along the arrow is primary; sideways misalignment is
         * penalized so navigation prefers the visually aligned neighbor. */
        double fwd = 0, side = 0;
        switch (dir) {
          case PT_PANE_DIR_LEFT:  fwd = cx - lx; side = ABS(ly - cy); break;
          case PT_PANE_DIR_RIGHT: fwd = lx - cx; side = ABS(ly - cy); break;
          case PT_PANE_DIR_UP:    fwd = cy - ly; side = ABS(lx - cx); break;
          case PT_PANE_DIR_DOWN:  fwd = ly - cy; side = ABS(lx - cx); break;
        }
        if (fwd > 0.5) {
          double score = fwd + 2.0 * side;
          if (best == NULL || score < best_score) {
            best = leaf;
            best_score = score;
          }
        }
      }
    }
    leaf = pt_split_next_leaf(g->tree, leaf);
  } while (leaf != NULL && leaf != first);

  if (best == NULL) return;
  g->focused = best;
  pt_pane_grid_focus_terminal(g);
  g_signal_emit(g, signals[SIG_FOCUS], 0);
  emit_focused_command(g);
}

/* Focus the pane with this id, if it is one of ours. Answers FALSE when it is
 * not, which is the ordinary case: the window asks every grid in turn, and a
 * notification whose pane was closed while it sat on screen finds no taker at
 * all. Emits the same signals a keyboard focus move does, so the status line
 * and the tab strip follow. */
static PtSplitNode *leaf_by_id(PtPaneGrid *g, guint64 id) {
  if (g->tree == NULL) return NULL;
  PtSplitNode *first = pt_split_first_leaf(g->tree);
  PtSplitNode *leaf = first;
  do {
    if (leaf != NULL && leaf->user != NULL &&
        pt_terminal_id(PT_TERMINAL(leaf->user)) == id)
      return leaf;
    leaf = pt_split_next_leaf(g->tree, leaf);
  } while (leaf != NULL && leaf != first);
  return NULL;
}

PtTerminal *pt_pane_grid_pane_by_id(PtPaneGrid *g, guint64 id) {
  PtSplitNode *leaf = leaf_by_id(g, id);
  return leaf != NULL ? PT_TERMINAL(leaf->user) : NULL;
}

gboolean pt_pane_grid_focus_pane_by_id(PtPaneGrid *g, guint64 id) {
  PtSplitNode *leaf = leaf_by_id(g, id);
  if (leaf == NULL) return FALSE;
  g->focused = leaf;
  pt_pane_grid_focus_terminal(g);
  g_signal_emit(g, signals[SIG_FOCUS], 0);
  emit_focused_command(g);
  return TRUE;
}

PtTerminal *pt_pane_grid_focused_terminal(PtPaneGrid *g) {
  return g->focused != NULL ? PT_TERMINAL(g->focused->user) : NULL;
}

static gboolean any_running_walk(PtSplitNode *n) {
  if (n == NULL) return FALSE;
  if (n->kind == PT_SPLIT_LEAF)
    return n->user != NULL && pt_terminal_running(PT_TERMINAL(n->user));
  return any_running_walk(n->a) || any_running_walk(n->b);
}

gboolean pt_pane_grid_any_running(PtPaneGrid *g) {
  return any_running_walk(g->tree);
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
      detach_from_parent(term);
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
  g_clear_pointer(&g->env, g_strfreev);
  G_OBJECT_CLASS(pt_pane_grid_parent_class)->dispose(obj);
}

static void pt_pane_grid_class_init(PtPaneGridClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_pane_grid_dispose;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass),
                                           GTK_TYPE_BIN_LAYOUT);
  signals[SIG_STRUCTURE] = g_signal_new("structure-changed", PT_TYPE_PANE_GRID,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_FOCUS] = g_signal_new("focus-changed", PT_TYPE_PANE_GRID,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_COMMAND] = g_signal_new("command-changed", PT_TYPE_PANE_GRID,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_TITLE] = g_signal_new("title-changed", PT_TYPE_PANE_GRID,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_NOTIFICATION] = g_signal_new("notification", PT_TYPE_PANE_GRID,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
      G_TYPE_UINT64, G_TYPE_STRING, G_TYPE_STRING);
  signals[SIG_EMPTIED] = g_signal_new("emptied", PT_TYPE_PANE_GRID,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void pt_pane_grid_init(PtPaneGrid *g) {
  gtk_widget_set_hexpand(GTK_WIDGET(g), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(g), TRUE);
}
