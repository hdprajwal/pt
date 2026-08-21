#include "pt-pane-grid.h"

#include "pt-agent.h"
#include "pt-agent-session.h"
#include "pt-zoom-state.h"

enum { SIG_STRUCTURE, SIG_FOCUS, SIG_COMMAND, SIG_TITLE,
       SIG_EMPTIED, SIG_NOTIFICATION, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _PtPaneGrid {
  GtkWidget parent_instance;
  PtSplitNode *tree;
  PtSplitNode *focused;   /* always a leaf of tree, or NULL when empty */
  GtkWidget *root_widget; /* current widget tree child */
  char **env;             /* child env for the panes we build, or NULL */
  /* What a pane we build starts out with, from the config. Held here rather
   * than in the widget file so that arming a new pane cannot touch the panes
   * that already exist — one of them may have been toggled by hand. */
  gboolean pane_mouse_reporting;
  PtOsc52Mode pane_osc52;
  /* Pane zoom (view-level, never saved): the focused pane fills the grid while
   * the other panes stay alive but hidden. See pt-zoom-state.h for the two
   * fields and their rules. */
  PtZoomState zoom;
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

/* The title is the tab's name while a program owns the pane — an agent renames
 * itself as its session goes on, and the window has no other edge to relabel
 * on. It also carries the previous command's exit code from the prompt, which
 * lands the instant the prompt is redrawn, well before the 700ms comm poll
 * notices the foreground program went back to the shell; re-emitting here lets
 * the window repaint "✗ exit 1" (and the tab dot) with no lag either. */
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
  /* Everything the pane needs from its window, and this pane only. The env goes
   * in before the pane is parented, so before it can allocate and spawn. */
  pt_terminal_set_spawn_env(PT_TERMINAL(term),
                            (const char *const *)g->env);
  pt_terminal_set_pane_mouse_reporting(PT_TERMINAL(term),
                                       g->pane_mouse_reporting);
  pt_terminal_set_pane_osc52(PT_TERMINAL(term), g->pane_osc52);
  /* A restored leaf that carried an agent session gets the resume command
   * queued for its first shell. Same window as the env: before the pane is
   * parented, so before it can allocate and spawn. */
  if (leaf->agent != NULL && leaf->agent_session != NULL) {
    char *cmd = pt_agent_session_resume_command(
        pt_agent_session_kind_from_name(leaf->agent), leaf->agent_session);
    if (cmd != NULL) pt_terminal_set_startup_input(PT_TERMINAL(term), cmd);
    g_free(cmd);
  }
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
  (void)spec;
  PtPaneGrid *g = PT_PANE_GRID(user);
  GtkPaned *paned = GTK_PANED(obj);
  /* Suspended while zoomed (and until the un-zoom restore finishes): hiding
   * and showing paned children reallocates the visible chain into shapes
   * nothing like the saved ratios, and the position writes those allocations
   * fire would clobber them. Un-zoom puts every ratio back before this
   * resumes — that is what makes a zoom round-trip lossless. */
  if (pt_zoom_state_sync_suspended(&g->zoom)) return;
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
  /* Any zoom state rode the old widget tree out with it — the fresh paneds
   * below re-apply every ratio through their own apply_ratio_tick passes and
   * arm their flags, so a split or close under a zoom lands on the real
   * layout by construction. Reset here, not in the callers: rebuild is the
   * one place every structural change goes through. */
  g->zoom = (PtZoomState){0};
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

GtkWidget *pt_pane_grid_new(PtSplitNode *tree, gboolean mouse_reporting,
                            PtOsc52Mode osc52) {
  PtPaneGrid *g = g_object_new(PT_TYPE_PANE_GRID, NULL);
  /* Set before rebuild(): it is what builds this grid's first panes. */
  g->pane_mouse_reporting = mouse_reporting;
  g->pane_osc52 = osc52;
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

void pt_pane_grid_set_pane_defaults(PtPaneGrid *g, gboolean mouse_reporting,
                                    PtOsc52Mode osc52) {
  /* Deliberately no walk over the panes that exist: the config apply that
   * carries a change does its own re-arm of every live pane, and every other
   * caller is only saying what the *next* pane should start out with. */
  g->pane_mouse_reporting = mouse_reporting;
  g->pane_osc52 = osc52;
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

/* ---------- pane zoom ---------- */
/* The grid is nested GtkPaneds (build_widgets above), so zoom is not a
 * split-tree edit but a visibility trick walked out at the widgets: every
 * paned on the path from the root down to the focused leaf hides its OPPOSITE
 * child. Hiding sibling leaves would collapse only the innermost paned and
 * leave every ancestor allocating by its stale pixel position — the focused
 * pane would grow, but not fill. The tree's ratios are never touched: position
 * syncing is suspended for the whole round-trip (see on_paned_position), and
 * un-zooming puts each saved ratio back through a tick before sync resumes,
 * so an un-zoom lands on exactly the layout there was before. Hidden panes
 * keep their PTYs at their last allocated size — GTK4 does not allocate hidden
 * widgets, so nothing resizes behind the user's back. */

/* Walk toward `leaf`, hiding each paned's opposite child on the way in.
 * TRUE when w's subtree holds the focused terminal. */
static gboolean hide_toward_leaf(GtkWidget *w, PtSplitNode *leaf) {
  if (w == NULL || leaf->user == NULL) return FALSE;
  if (!GTK_IS_PANED(w)) return w == GTK_WIDGET(leaf->user);
  PtSplitNode *node = g_object_get_data(G_OBJECT(w), "pt-node");
  if (node == NULL) return FALSE;
  GtkWidget *start = gtk_paned_get_start_child(GTK_PANED(w));
  GtkWidget *end = gtk_paned_get_end_child(GTK_PANED(w));
  if (pt_split_contains(node->a, leaf) && hide_toward_leaf(start, leaf)) {
    gtk_widget_set_visible(end, FALSE);
    return TRUE;
  }
  if (pt_split_contains(node->b, leaf) && hide_toward_leaf(end, leaf)) {
    gtk_widget_set_visible(start, FALSE);
    return TRUE;
  }
  return FALSE;
}

/* Both children of every paned back on. A full sweep rather than just the zoom
 * path because it costs nothing and forgives everything the path might have
 * missed; terminals are never individually hidden anywhere else. */
static void show_everything(GtkWidget *w) {
  if (w == NULL || !GTK_IS_PANED(w)) return;
  GtkPaned *p = GTK_PANED(w);
  GtkWidget *start = gtk_paned_get_start_child(p);
  GtkWidget *end = gtk_paned_get_end_child(p);
  gtk_widget_set_visible(start, TRUE);
  gtk_widget_set_visible(end, TRUE);
  show_everything(start);
  show_everything(end);
}

/* One frame after un-zoom: put this paned's saved ratio back. Same shape as
 * apply_ratio_tick above — a tick, not an idle, because a just-shown child has
 * no size until a frame allocates one — and "pt-ratio-applied" only arms after
 * the write, so GTK's own reallocation passes cannot sneak a degenerate split
 * into the tree on the way back. Zoom itself is already off by the time any of
 * these run; each one just drains the pending counter, and syncing resumes
 * when the last lands. */
static gboolean restore_ratio_tick(GtkWidget *w, GdkFrameClock *clock,
                                   gpointer data) {
  (void)clock;
  PtPaneGrid *g = PT_PANE_GRID(data);
  PtSplitNode *node = g_object_get_data(G_OBJECT(w), "pt-node");
  if (node != NULL && node->kind != PT_SPLIT_LEAF) {
    int total = (node->kind == PT_SPLIT_H) ? gtk_widget_get_width(w)
                                           : gtk_widget_get_height(w);
    if (total <= 0) {
      /* Not allocated yet. Patience is bounded: a window minimized for the
       * whole restore never sizes its paneds, and waiting forever would keep
       * the pending counter up and divider syncing suspended for good. After
       * a fixed run of starved frames this paned gives up: no position write
       * (there is no size to compute one from) and no arming, so the saved
       * ratio stays untouched in the tree for the next rebuild rather than
       * being clobbered by whatever degenerate allocation GTK guessed. */
      guint starved = GPOINTER_TO_UINT(
          g_object_get_data(G_OBJECT(w), "pt-restore-starved"));
      if (!pt_zoom_state_tick_starved(++starved)) {
        g_object_set_data(G_OBJECT(w), "pt-restore-starved",
                          GUINT_TO_POINTER(starved));
        return G_SOURCE_CONTINUE;
      }
    } else {
      gtk_paned_set_position(GTK_PANED(w), (int)(total * node->ratio));
      g_object_set_data(G_OBJECT(w), "pt-ratio-applied", GINT_TO_POINTER(1));
    }
    g_object_set_data(G_OBJECT(w), "pt-restore-starved", NULL);
  }
  /* node NULL means torn down mid-restore: count it anyway so the pending
   * counter can never wedge on. */
  pt_zoom_state_restore_done(&g->zoom);
  return G_SOURCE_REMOVE;
}

static void arm_ratio_restore(GtkWidget *w, PtPaneGrid *g) {
  if (w == NULL || !GTK_IS_PANED(w)) return;
  g_object_set_data(G_OBJECT(w), "pt-ratio-applied", NULL);
  g->zoom.ratio_restores++;
  gtk_widget_add_tick_callback(w, restore_ratio_tick, g, NULL);
  arm_ratio_restore(gtk_paned_get_start_child(GTK_PANED(w)), g);
  arm_ratio_restore(gtk_paned_get_end_child(GTK_PANED(w)), g);
}

/* Restore the real layout after a leave: every hidden child back on, then a
 * tick per paned to re-apply the saved ratios. Zoom itself ended when the
 * state said so — the chip and the toggle see it at once — while divider
 * syncing stays suspended until the last pending tick lands. Idempotent by
 * construction: once zoom is off this returns without touching anything, so
 * key repeat or an unzoom from a focus move racing the next frame cannot arm
 * a second sweep against the pending counter. */
static void unzoom_pending(PtPaneGrid *g) {
  if (!pt_zoom_state_leave(&g->zoom)) return;
  show_everything(g->root_widget);
  /* Arms one tick per paned; nothing at all when root is missing or a lone
   * terminal (no paneds to restore) — then the counter simply stays at 0 and
   * nothing pends. */
  arm_ratio_restore(g->root_widget, g);
}

gboolean pt_pane_grid_toggle_zoom(PtPaneGrid *g) {
  g_return_val_if_fail(PT_IS_PANE_GRID(g), FALSE);
  /* A single-pane tab fills the grid already; an empty one has no focus. */
  gboolean can_zoom =
      g->focused != NULL && pt_split_count_leaves(g->tree) >= 2;
  switch (pt_zoom_state_toggle(&g->zoom, can_zoom)) {
    case PT_ZOOM_TOGGLE_ENTER:
      /* Only really zoomed once the widgets hid: no focused terminal to
       * walk to means the state flips straight back. */
      if (!hide_toward_leaf(g->root_widget, g->focused)) {
        pt_zoom_state_leave(&g->zoom);
        return FALSE;
      }
      return TRUE;
    case PT_ZOOM_TOGGLE_LEAVE:
      show_everything(g->root_widget);
      arm_ratio_restore(g->root_widget, g);
      return FALSE;
    case PT_ZOOM_TOGGLE_IGNORED:
      /* Single-pane or empty grid, or mid-un-zoom (restore ticks pending):
       * already on the way out, and arming a second sweep would double-count
       * the pending counter. */
      return FALSE;
  }
  g_assert_not_reached();
}

void pt_pane_grid_unzoom(PtPaneGrid *g) {
  g_return_if_fail(PT_IS_PANE_GRID(g));
  unzoom_pending(g);
}

gboolean pt_pane_grid_get_zoomed(PtPaneGrid *g) {
  g_return_val_if_fail(PT_IS_PANE_GRID(g), FALSE);
  return pt_zoom_state_active(&g->zoom);
}

void pt_pane_grid_focus_next(PtPaneGrid *g) {
  if (g->focused == NULL) return;
  /* Policy: an explicit focus move always leaves zoom first. Skipping hidden
   * leaves would cycle focus through panes the user cannot see; landing on
   * one silently would move focus nowhere visible. Un-zooming shows the pane
   * the move lands on — the grid comes back with it. */
  pt_pane_grid_unzoom(g);
  g->focused = pt_split_next_leaf(g->tree, g->focused);
  pt_pane_grid_focus_terminal(g);
  g_signal_emit(g, signals[SIG_FOCUS], 0);
  emit_focused_command(g);
}

void pt_pane_grid_focus_prev(PtPaneGrid *g) {
  if (g->focused == NULL) return;
  pt_pane_grid_unzoom(g);   /* same policy as focus_next */
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
  /* Same policy as focus_next: a directional move leaves zoom first, so the
   * geometry below sees the real layout — and every pane is visible to land
   * on. */
  pt_pane_grid_unzoom(g);
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

/* The pane whose core was handed this PT_PANE_TOKEN — the same walk
 * sync_agent_walk does, answering "which pane owns this agent report" for the
 * window's report watcher. A leaf without a live core has no token to match,
 * which is the right answer for a pane that never spawned. */
static PtSplitNode *leaf_by_token(PtPaneGrid *g, const char *token) {
  if (g->tree == NULL || token == NULL) return NULL;
  PtSplitNode *first = pt_split_first_leaf(g->tree);
  PtSplitNode *leaf = first;
  do {
    if (leaf != NULL && leaf->user != NULL) {
      PtTermCore *core = pt_terminal_core(PT_TERMINAL(leaf->user));
      const char *tok = core != NULL ? pt_term_core_pane_token(core) : NULL;
      if (tok != NULL && g_strcmp0(tok, token) == 0) return leaf;
    }
    leaf = pt_split_next_leaf(g->tree, leaf);
  } while (leaf != NULL && leaf != first);
  return NULL;
}

PtTerminal *pt_pane_grid_pane_by_token(PtPaneGrid *g, const char *token) {
  PtSplitNode *leaf = leaf_by_token(g, token);
  return leaf != NULL ? PT_TERMINAL(leaf->user) : NULL;
}

gboolean pt_pane_grid_focus_pane_by_id(PtPaneGrid *g, guint64 id) {
  PtSplitNode *leaf = leaf_by_id(g, id);
  if (leaf == NULL) return FALSE;
  pt_pane_grid_unzoom(g);   /* a clicked notification asks for its pane shown */
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

static void sync_agent_walk(PtSplitNode *n) {
  if (n == NULL) return;
  if (n->kind != PT_SPLIT_LEAF) {
    sync_agent_walk(n->a);
    sync_agent_walk(n->b);
    return;
  }
  /* Only a leaf with a live core gets recomputed. A leaf whose pane was never
   * built, or built but never spawned, has run nothing that could have ended
   * the agent session it was restored with — clearing its fields here would
   * throw away the resume of a tab the user simply has not opened yet. The
   * fields are only ever overwritten by evidence from a shell that actually
   * ran. */
  if (n->user == NULL) return;
  PtTermCore *core = pt_terminal_core(PT_TERMINAL(n->user));
  if (core == NULL) return;
  pt_split_leaf_set_agent(n, NULL, NULL);
  const char *token = pt_term_core_pane_token(core);
  if (token == NULL) return;
  char *path = pt_agent_session_report_path(token);
  PtAgentSessionReport *r = pt_agent_session_report_load(path);
  g_free(path);
  if (r == NULL) return;
  /* NULL fg_name forces the /proc walk so a pid comes back — the fast path
   * answers kind-only. The walk is bounded and the save is debounced, so this
   * stays off any hot path. */
  int pid = 0;
  PtAgentKind kind =
      pt_agent_detect((int)pt_term_core_shell_pid(core), NULL, &pid);
  if (pt_agent_session_report_matches(r, kind, pid))
    pt_split_leaf_set_agent(n, pt_agent_session_kind_name(r->agent),
                            r->session_id);
  pt_agent_session_report_free(r);
}

void pt_pane_grid_sync_agents(PtPaneGrid *g) { sync_agent_walk(g->tree); }

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
  /* Overwritten by pt_pane_grid_new before it builds a single pane; here so a
   * grid can never hand a pane a zero-initialized "config". */
  g->pane_mouse_reporting = PT_CONFIG_MOUSE_REPORTING_DEFAULT;
  g->pane_osc52 = PT_CONFIG_OSC52_DEFAULT;
  gtk_widget_set_hexpand(GTK_WIDGET(g), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(g), TRUE);
}
