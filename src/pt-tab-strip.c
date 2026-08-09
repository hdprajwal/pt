#include "pt-tab-strip.h"

#include "pt-rowlist.h"

enum { SIG_SELECTED, SIG_NEW, SIG_CLOSE, SIG_MOVED, SIG_OPEN_EDITOR,
       SIG_TOGGLE_PANEL, N_SIGNALS };
static guint signals[N_SIGNALS];

typedef struct TabSnapshot TabSnapshot;

struct _PtTabStrip {
  GtkWidget parent_instance;
  /* horizontal, three children: the rowlist's tabs box, an expanding filler,
   * and the actions box holding +, zed and panel. The buttons sit inside that
   * third box rather than in `box` directly, so a tab rebuild — which clears
   * the rowlist's box — can never destroy them mid-click. */
  GtkWidget *box;
  /* The tabs. The row list owns the last-rendered snapshot and skips a rebuild
   * when nothing in it moved: output-driven refreshes fire several times a
   * second, and a button destroyed between press and release cancels its
   * gesture — the click is silently swallowed. */
  PtRowList *tabs;
  /* The box the row list renders into, kept so a title change can be written
   * straight onto the labels already on screen instead of going through a
   * rebuild. Owned by `box` — dispose unparents that and these go with it. */
  GtkWidget *tabs_host;
  /* Probed once in init, not per rebuild: the strip rebuilds several times a
   * second and PATH does not move underneath a running window. */
  gboolean has_zed;
  gboolean dragging;       /* a tab drag is in flight (see set_tabs) */
  int drag_from;           /* tab being dragged, -1 when idle */
  int drop_from, drop_to;  /* move a drop asked for, -1 for none yet */
  TabSnapshot *pending;    /* newest data a drag held back, NULL for none */
};

G_DEFINE_FINAL_TYPE(PtTabStrip, pt_tab_strip, GTK_TYPE_WIDGET)

/* ---------- the rendered snapshot ---------- */
/* Everything one rebuild draws from: the tabs plus the two indices that decide
 * how they look. `accent` never reaches a row, but a change in it does mean the
 * strip is showing another project, so it belongs in the comparison. */
struct TabSnapshot {
  PtTabInfo *tabs;   /* deep copy; title is g_strdup'd */
  int n, active, accent;
};

static TabSnapshot *snapshot_new(const PtTabInfo *tabs, int n, int active,
                                 int accent) {
  TabSnapshot *s = g_new0(TabSnapshot, 1);
  s->active = active;
  s->accent = accent;
  if (n > 0) {
    s->tabs = g_new0(PtTabInfo, n);
    s->n = n;
    for (int i = 0; i < n; i++) {
      s->tabs[i] = tabs[i];
      s->tabs[i].title = g_strdup(tabs[i].title);
    }
  }
  return s;
}

static void snapshot_free(gpointer data) {
  TabSnapshot *s = data;
  for (int i = 0; i < s->n; i++) g_free((char *)s->tabs[i].title);
  g_free(s->tabs);
  g_free(s);
}

static gboolean snapshot_equal(gpointer ap, guint na, gpointer bp, guint nb,
                               gpointer u) {
  (void)u;
  const TabSnapshot *a = ap, *b = bp;
  if (a == NULL || b == NULL || na != nb) return FALSE;
  if (a->active != b->active || a->accent != b->accent) return FALSE;
  /* Titles are deliberately not compared. A tab is named after the title the
   * program in it set, and a coding agent animates a spinner in that title
   * about once a second — comparing them would report "moved" at exactly the
   * rate this dedupe exists to keep rebuilds away from, and every close button
   * in the strip would be destroyed between a press and its release. The set
   * caller writes new titles onto the existing labels instead. */
  for (guint i = 0; i < na; i++) {
    if (a->tabs[i].running != b->tabs[i].running ||
        a->tabs[i].last_exit != b->tabs[i].last_exit)
      return FALSE;
  }
  return TRUE;
}

/* ---------- callbacks ---------- */
/* The tab is a box, not a button: a GtkButton nested in a GtkButton is fragile,
 * so selection rides a click gesture (same shape as the sidebar's project row)
 * and the × stays a real button.
 *
 * A press on the × must NOT also select: selecting rebuilds the strip on the
 * spot, and a button destroyed between press and release never emits "clicked"
 * (the same hazard the rebuild dedupe exists for) — the close would be eaten by
 * the switch. The button does not claim the sequence early enough to stop this
 * gesture, so hit-test the press and stand down when it landed on the ×. The
 * separator that rides in the same slot is not part of the tab either. Where the
 * press landed is only knowable here, so the verdict is remembered for release
 * rather than acted on. */
static void on_tab_pressed(GtkGestureClick *g, int n, double x, double y,
                           gpointer user) {
  (void)n; (void)user;
  GtkWidget *slot = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
  GtkWidget *tab = g_object_get_data(G_OBJECT(slot), "pt-tab");
  GtkWidget *close = g_object_get_data(G_OBJECT(slot), "pt-close");
  GtkWidget *hit = gtk_widget_pick(slot, x, y, GTK_PICK_DEFAULT);
  gboolean on_tab = hit != NULL && tab != NULL &&
                    (hit == tab || gtk_widget_is_ancestor(hit, tab)) &&
                    !(close != NULL &&
                      (hit == close || gtk_widget_is_ancestor(hit, close)));
  g_object_set_data(G_OBJECT(g), "pt-armed", GINT_TO_POINTER(on_tab));
  g_object_set_data(G_OBJECT(g), "pt-press-x", GINT_TO_POINTER((int)x));
  g_object_set_data(G_OBJECT(g), "pt-press-y", GINT_TO_POINTER((int)y));
}

/* Release, not press, exactly as the sidebar's rows do it: selecting on press
 * switches tab — and the window answers that by rebuilding the strip,
 * destroying this very slot — before the drag source has seen enough motion to
 * recognise a drag, so tabs would be undraggable. A recognised drag never
 * reaches this handler: gtk_drag_source_drag_begin() resets the slot's
 * controllers, which drops this gesture's sequence. */
static void on_tab_released(GtkGestureClick *g, int n, double x, double y,
                            gpointer user) {
  (void)n;
  GtkWidget *slot = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
  if (!GPOINTER_TO_INT(g_object_get_data(G_OBJECT(g), "pt-armed"))) return;
  /* A drag that never started still ends up here: GtkDragSource ignores motion
   * for the first 100ms of a press (MIN_TIME_TO_DND), so a quick flick crosses
   * the drag threshold without ever beginning a drag. Measure the travel with
   * GTK's own threshold and let that release go, otherwise an abandoned drag
   * switches tab. */
  if (gtk_drag_check_threshold(
          slot, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(g), "pt-press-x")),
          GPOINTER_TO_INT(g_object_get_data(G_OBJECT(g), "pt-press-y")),
          (int)x, (int)y))
    return;
  g_signal_emit(PT_TAB_STRIP(user), signals[SIG_SELECTED], 0,
                pt_rowlist_row_index(slot));
}

static void on_tab_close_clicked(GtkButton *btn, gpointer user) {
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "pt-index"));
  g_signal_emit(PT_TAB_STRIP(user), signals[SIG_CLOSE], 0, idx);
}

static void on_new_clicked(GtkButton *btn, gpointer user) {
  (void)btn;
  g_signal_emit(PT_TAB_STRIP(user), signals[SIG_NEW], 0);
}

static void on_zed_clicked(GtkButton *btn, gpointer user) {
  (void)btn;
  g_signal_emit(PT_TAB_STRIP(user), signals[SIG_OPEN_EDITOR], 0);
}

static void on_panel_clicked(GtkButton *btn, gpointer user) {
  (void)btn;
  g_signal_emit(PT_TAB_STRIP(user), signals[SIG_TOGGLE_PANEL], 0);
}

/* ---------- drag to reorder ---------- */
static GtkWidget *build_tab(gpointer items, guint idx, gpointer u);

/* One rebuild, from a snapshot this takes ownership of. */
static void render_tabs(PtTabStrip *s, TabSnapshot *snap) {
  pt_rowlist_set(s->tabs, snap, snap->n > 0 ? (guint)snap->n : 0, build_tab,
                 snapshot_equal, s, snapshot_free);
}

/* The drop marks ride the tab itself, not the slot: the slot also holds the
 * separator, and a marker drawn across that would sit in the next tab's gutter. */
static GtkWidget *slot_tab(GtkWidget *slot) {
  return g_object_get_data(G_OBJECT(slot), "pt-tab");
}

static void clear_drop_marks(PtTabStrip *s) {
  if (s->tabs_host == NULL) return;
  for (GtkWidget *slot = gtk_widget_get_first_child(s->tabs_host);
       slot != NULL; slot = gtk_widget_get_next_sibling(slot)) {
    GtkWidget *tab = slot_tab(slot);
    if (tab == NULL) continue;
    gtk_widget_remove_css_class(tab, "pt-drop-left");
    gtk_widget_remove_css_class(tab, "pt-drop-right");
  }
}

/* Where the dragged tab would land if it were dropped at `x` on this slot, in
 * the index space pt_workspace_move_tab speaks: the half of the tab the pointer
 * is in says before or after it, and the steal that precedes the insert shifts
 * everything past the dragged tab down one. -1 when that is where it already
 * is, which is not a move. */
static int drop_target_index(PtTabStrip *s, GtkWidget *slot, double x,
                             gboolean *after) {
  int idx = pt_rowlist_row_index(slot);
  if (idx < 0 || s->drag_from < 0) return -1;
  gboolean right = x > gtk_widget_get_width(slot) / 2.0;
  int want = right ? idx + 1 : idx;
  int to = want > s->drag_from ? want - 1 : want;
  if (to == s->drag_from) return -1;
  if (after != NULL) *after = right;
  return to;
}

static GdkContentProvider *on_drag_prepare(GtkDragSource *src, double x,
                                           double y, gpointer user) {
  (void)user;
  /* The hotspot has to come from here; ::drag-begin gets no coordinates. */
  g_object_set_data(G_OBJECT(src), "pt-hot-x", GINT_TO_POINTER((int)x));
  g_object_set_data(G_OBJECT(src), "pt-hot-y", GINT_TO_POINTER((int)y));
  GtkWidget *slot = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(src));
  return gdk_content_provider_new_typed(G_TYPE_INT,
                                        pt_rowlist_row_index(slot));
}

static void on_drag_begin(GtkDragSource *src, GdkDrag *drag, gpointer user) {
  (void)drag;
  PtTabStrip *s = user;
  GtkWidget *slot = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(src));
  s->dragging = TRUE;
  s->drag_from = pt_rowlist_row_index(slot);
  s->drop_from = s->drop_to = -1;
  GdkPaintable *icon = gtk_widget_paintable_new(slot_tab(slot));
  gtk_drag_source_set_icon(src, icon,
      GPOINTER_TO_INT(g_object_get_data(G_OBJECT(src), "pt-hot-x")),
      GPOINTER_TO_INT(g_object_get_data(G_OBJECT(src), "pt-hot-y")));
  g_object_unref(icon);
}

/* GtkDragSource emits this after a cancelled or failed drag too, so it is the
 * one place that can be trusted to lift the rebuild freeze — miss that and the
 * strip stops updating for the rest of the session. */
static void on_drag_end(GtkDragSource *src, GdkDrag *drag, gboolean del,
                        gpointer user) {
  (void)src; (void)drag; (void)del;
  PtTabStrip *s = user;
  /* A GtkDragSource keeps itself alive until the drag ends, so this can arrive
   * after the strip took its tabs down (the window closing mid-drag). */
  if (s->tabs_host == NULL) return;
  s->dragging = FALSE;
  s->drag_from = -1;
  clear_drop_marks(s);
  int from = s->drop_from, to = s->drop_to;
  s->drop_from = s->drop_to = -1;
  /* Deferred out of ::drop on purpose: the window answers by rebuilding every
   * tab, and the dragged widget has to outlive GTK's drop handling. */
  if (from >= 0 && to >= 0)
    g_signal_emit(s, signals[SIG_MOVED], 0, from, to);
  /* Refreshes that arrived mid-drag were held back; draw the newest data once.
   * A move above normally does this already, through the window's refresh —
   * which clears `pending` as it renders. */
  if (s->pending != NULL) {
    TabSnapshot *snap = s->pending;
    s->pending = NULL;
    render_tabs(s, snap);
  }
}

/* enter and motion both: the mark follows the pointer across the tab, so it
 * cannot be decided once on the way in. */
static GdkDragAction on_drop_motion(GtkDropTarget *dt, double x, double y,
                                    gpointer user) {
  (void)y;
  PtTabStrip *s = user;
  GtkWidget *slot = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(dt));
  GtkWidget *tab = slot_tab(slot);
  if (tab == NULL) return 0;
  gboolean after = FALSE;
  if (drop_target_index(s, slot, x, &after) < 0) {
    gtk_widget_remove_css_class(tab, "pt-drop-left");
    gtk_widget_remove_css_class(tab, "pt-drop-right");
    return 0;
  }
  gtk_widget_remove_css_class(tab, after ? "pt-drop-left" : "pt-drop-right");
  gtk_widget_add_css_class(tab, after ? "pt-drop-right" : "pt-drop-left");
  return GDK_ACTION_MOVE;
}

static void on_drop_leave(GtkDropTarget *dt, gpointer user) {
  (void)user;
  GtkWidget *tab =
      slot_tab(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(dt)));
  if (tab == NULL) return;
  gtk_widget_remove_css_class(tab, "pt-drop-left");
  gtk_widget_remove_css_class(tab, "pt-drop-right");
}

static gboolean on_drop(GtkDropTarget *dt, const GValue *value, double x,
                        double y, gpointer user) {
  (void)y;
  PtTabStrip *s = user;
  if (!G_VALUE_HOLDS_INT(value)) return FALSE;
  int from = g_value_get_int(value);
  /* Only the drag this strip started: the payload is a bare int, and another
   * window's tab index would name a tab that is not ours. */
  if (from < 0 || from != s->drag_from) return FALSE;
  GtkWidget *slot = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(dt));
  int to = drop_target_index(s, slot, x, NULL);
  if (to < 0) return FALSE;
  s->drop_from = from;
  s->drop_to = to;
  return TRUE;
}

/* ---------- one tab ---------- */
/* Two widgets per tab, so they travel as one slot: the tab itself and the
 * full-height 1px border after it (Zed-style, the active tab included). */
static GtkWidget *build_tab(gpointer items, guint idx, gpointer u) {
  const TabSnapshot *snap = items;
  PtTabStrip *s = u;
  const PtTabInfo *info = &snap->tabs[idx];

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
  gtk_widget_add_css_class(row, "pt-tab");
  if ((int)idx == snap->active) gtk_widget_add_css_class(row, "active");

  GtkWidget *dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(dot, "pt-dot");
  gtk_widget_add_css_class(dot, "pt-dot-6");
  gtk_widget_add_css_class(dot, info->running          ? "running"
                                : info->last_exit > 0  ? "error"
                                                       : "idle");
  gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(row), dot);

  GtkWidget *lbl = gtk_label_new(info->title != NULL ? info->title : "");
  gtk_widget_add_css_class(lbl, "pt-tab-label");
  gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars(GTK_LABEL(lbl), 24);
  gtk_box_append(GTK_BOX(row), lbl);

  /* Hidden (opacity 0) until the tab is hovered — see .pt-tab-close in
   * style.css. It closes the whole tab, panes and all. */
  GtkWidget *close = gtk_button_new_with_label("×");
  gtk_widget_add_css_class(close, "flat");
  gtk_widget_add_css_class(close, "pt-tab-close");
  gtk_widget_set_valign(close, GTK_ALIGN_CENTER);
  g_object_set_data(G_OBJECT(close), "pt-index", GINT_TO_POINTER((int)idx));
  g_signal_connect(close, "clicked", G_CALLBACK(on_tab_close_clicked), s);
  gtk_box_append(GTK_BOX(row), close);

  GtkWidget *slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append(GTK_BOX(slot), row);
  GtkWidget *sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(sep, "pt-tab-sep");
  gtk_box_append(GTK_BOX(slot), sep);
  g_object_set_data(G_OBJECT(slot), "pt-tab", row);
  g_object_set_data(G_OBJECT(slot), "pt-close", close);
  /* Reachable from the slot so a later set can retitle this tab in place. */
  g_object_set_data(G_OBJECT(slot), "pt-label", lbl);

  /* The gesture goes on the slot, not the tab: the row list carries the tab
   * index there, and the hit-test above sorts out where the press landed. */
  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click, "pressed", G_CALLBACK(on_tab_pressed), s);
  g_signal_connect(click, "released", G_CALLBACK(on_tab_released), s);
  gtk_widget_add_controller(slot, GTK_EVENT_CONTROLLER(click));

  /* connect_object, unlike the click gesture above: a drag source outlives its
   * slot for the length of the drag, so its handlers must go quiet if the strip
   * itself is gone by the time the drag ends. */
  GtkDragSource *src = gtk_drag_source_new();
  gtk_drag_source_set_actions(src, GDK_ACTION_MOVE);
  g_signal_connect_object(src, "prepare", G_CALLBACK(on_drag_prepare), s, 0);
  g_signal_connect_object(src, "drag-begin", G_CALLBACK(on_drag_begin), s, 0);
  g_signal_connect_object(src, "drag-end", G_CALLBACK(on_drag_end), s, 0);
  gtk_widget_add_controller(slot, GTK_EVENT_CONTROLLER(src));

  GtkDropTarget *dst = gtk_drop_target_new(G_TYPE_INT, GDK_ACTION_MOVE);
  g_signal_connect(dst, "enter", G_CALLBACK(on_drop_motion), s);
  g_signal_connect(dst, "motion", G_CALLBACK(on_drop_motion), s);
  g_signal_connect(dst, "leave", G_CALLBACK(on_drop_leave), s);
  g_signal_connect(dst, "drop", G_CALLBACK(on_drop), s);
  gtk_widget_add_controller(slot, GTK_EVENT_CONTROLLER(dst));
  return slot;
}

/* Full rebuild whenever the state moved: at ≤ ~10 tabs that is cheaper than
 * tracking per-child widgets, and the strip already rebuilt on every tab
 * switch. Identical state is a no-op (see the snapshot rationale above). */
void pt_tab_strip_set_tabs(PtTabStrip *s, const PtTabInfo *tabs, int n,
                           int active, int accent) {
  TabSnapshot *snap = snapshot_new(tabs, n, active, accent);
  /* Rebuilding mid-drag would destroy the very widget GTK is dragging, and one
   * busy pane pushes refreshes through here a couple of times a second. Keep
   * the newest data and let drag-end draw it; the titles below still land on
   * the tabs already on screen. */
  g_clear_pointer(&s->pending, snapshot_free);
  if (s->dragging) s->pending = snap;
  else             render_tabs(s, snap);

  /* The one piece of state a rebuild is not allowed to carry, applied by hand.
   * Correct either way: a rebuild built its labels from this same array, so
   * every write below lands on text already equal to it; without one, this is
   * the only thing that moves the titles at all. gtk_label_set_text drops a
   * write that changes nothing, so a strip nobody is animating costs a string
   * compare per tab and no relayout. */
  if (s->tabs_host == NULL) return;
  for (GtkWidget *slot = gtk_widget_get_first_child(s->tabs_host);
       slot != NULL; slot = gtk_widget_get_next_sibling(slot)) {
    GtkWidget *lbl = g_object_get_data(G_OBJECT(slot), "pt-label");
    if (lbl == NULL) continue;
    /* The row list's index, not the position: a build_row that filtered an
     * item out would leave the two disagreeing. */
    int idx = pt_rowlist_row_index(slot);
    if (idx < 0 || idx >= n) continue;
    gtk_label_set_text(GTK_LABEL(lbl),
                       tabs[idx].title != NULL ? tabs[idx].title : "");
  }
}

static void pt_tab_strip_dispose(GObject *obj) {
  PtTabStrip *s = PT_TAB_STRIP(obj);
  /* Rows before the row list: a tab must never outlive the list its gesture
   * points at. The tabs box is only borrowed — `box` owns it and takes it down
   * on the next line — so clear the pointer first, or a set arriving after this
   * would walk freed children. Same reason the row list drops its own host. */
  s->tabs_host = NULL;
  g_clear_pointer(&s->box, gtk_widget_unparent);
  g_clear_object(&s->tabs);
  g_clear_pointer(&s->pending, snapshot_free);
  G_OBJECT_CLASS(pt_tab_strip_parent_class)->dispose(obj);
}

static void pt_tab_strip_class_init(PtTabStripClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_tab_strip_dispose;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass),
                                           GTK_TYPE_BIN_LAYOUT);
  signals[SIG_SELECTED] = g_signal_new("tab-selected", PT_TYPE_TAB_STRIP,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
  signals[SIG_NEW] = g_signal_new("tab-new", PT_TYPE_TAB_STRIP,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_CLOSE] = g_signal_new("tab-close", PT_TYPE_TAB_STRIP,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
  signals[SIG_MOVED] = g_signal_new("tab-moved", PT_TYPE_TAB_STRIP,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2,
      G_TYPE_INT, G_TYPE_INT);
  signals[SIG_OPEN_EDITOR] = g_signal_new("open-editor", PT_TYPE_TAB_STRIP,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_TOGGLE_PANEL] = g_signal_new("toggle-panel", PT_TYPE_TAB_STRIP,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void pt_tab_strip_init(PtTabStrip *s) {
  gtk_widget_add_css_class(GTK_WIDGET(s), "pt-tabstrip");
  s->drag_from = s->drop_from = s->drop_to = -1;
  char *zed = g_find_program_in_path("zed");
  s->has_zed = (zed != NULL);
  g_free(zed);
  /* Fill, not centre (GTK_ALIGN_FILL is the default): the strip's bottom rule
   * lives on the tabs now, so a centred box would float them mid-strip and
   * draw the line in the wrong place. The buttons keep their own valign. */
  s->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_parent(s->box, GTK_WIDGET(s));

  /* The tabs get a box of their own: the cluster on the right is built once
   * here, and a rebuild that cleared it would destroy those buttons several
   * times a second — mid-click, given the chance. */
  s->tabs_host = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append(GTK_BOX(s->box), s->tabs_host);
  s->tabs = pt_rowlist_new(GTK_BOX(s->tabs_host));

  /* Empty expanding box so the + sits at the strip's right edge, like Zed. It
   * carries the bottom rule across the strip's empty run — see
   * `.pt-tabstrip-filler` in style.css. */
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_widget_add_css_class(spacer, "pt-tabstrip-filler");
  gtk_box_append(GTK_BOX(s->box), spacer);

  /* One box for the right-hand cluster, so the bottom rule can run across it
   * too: with the border on the strip's children rather than the strip, every
   * child that is not a tab has to carry it or the line breaks. */
  GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(actions, "pt-tab-actions");
  gtk_box_append(GTK_BOX(s->box), actions);

  GtkWidget *plus = gtk_button_new_with_label("+");
  gtk_widget_add_css_class(plus, "flat");
  gtk_widget_add_css_class(plus, "pt-tab-new");
  gtk_widget_set_valign(plus, GTK_ALIGN_CENTER);
  g_signal_connect(plus, "clicked", G_CALLBACK(on_new_clicked), s);
  gtk_box_append(GTK_BOX(actions), plus);

  /* Opens the active project in Zed. Omitted entirely when zed is not on
   * PATH — a button that cannot do anything is worse than no button. */
  if (s->has_zed) {
    GtkWidget *zed_btn = gtk_button_new_from_icon_name("pt-zed-symbolic");
    gtk_widget_add_css_class(zed_btn, "flat");
    gtk_widget_add_css_class(zed_btn, "pt-tab-zed");
    gtk_widget_set_valign(zed_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(zed_btn, "Open in Zed");
    g_signal_connect(zed_btn, "clicked", G_CALLBACK(on_zed_clicked), s);
    gtk_box_append(GTK_BOX(actions), zed_btn);
  }

  /* Last in the cluster: toggles the info panel, same as ⌃I. Unlike the Zed
   * button this is never gated — the panel is always there to show. */
  GtkWidget *panel = gtk_button_new_from_icon_name("pt-panel-right-symbolic");
  gtk_widget_add_css_class(panel, "flat");
  gtk_widget_add_css_class(panel, "pt-tab-panel");
  gtk_widget_set_valign(panel, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(panel, "Toggle info panel  ^I");
  g_signal_connect(panel, "clicked", G_CALLBACK(on_panel_clicked), s);
  gtk_box_append(GTK_BOX(actions), panel);
}

GtkWidget *pt_tab_strip_new(void) {
  return g_object_new(PT_TYPE_TAB_STRIP, NULL);
}
