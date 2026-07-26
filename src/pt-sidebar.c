#include "pt-sidebar.h"
#include "pt-fuzzy.h"
#include "pt-accent.h"

/* The sidebar is a fixed-width rail, not a min-width one. */
#define PT_SIDEBAR_WIDTH 266

enum { SIG_SELECTED, SIG_ADD, SIG_REMOVE, SIG_SEARCH_ESCAPE, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _PtSidebar {
  GtkWidget parent_instance;
  GtkWidget *box;          /* vertical: search, rows, sep, add */
  GtkWidget *search;       /* GtkText */
  GtkWidget *rows_box;
  PtSidebarRow *rows;      /* owned deep copy; name/path are g_strdup'd */
  int n_rows;
  int active;
  gboolean rendered;       /* rows_box reflects `rows` (see set_projects) */
  char *query;             /* owned; NULL or "" means "show everything" */
};

G_DEFINE_FINAL_TYPE(PtSidebar, pt_sidebar, GTK_TYPE_WIDGET)

/* ---------- owned row storage ---------- */
static void clear_rows(PtSidebar *sb) {
  for (int i = 0; i < sb->n_rows; i++) {
    g_free((char *)sb->rows[i].name);
    g_free((char *)sb->rows[i].path);
  }
  g_clear_pointer(&sb->rows, g_free);
  sb->n_rows = 0;
}

/* ---------- helpers ---------- */
static gboolean row_matches(PtSidebar *sb, int i) {
  if (sb->query == NULL || sb->query[0] == '\0') return TRUE;
  return pt_fuzzy_score(sb->query, sb->rows[i].name) != 0 ||
         pt_fuzzy_score(sb->query, sb->rows[i].path) != 0;
}

/* First row that survives the current filter, in original index space.
 * -1 when nothing matches. */
static int first_visible_index(PtSidebar *sb) {
  for (int i = 0; i < sb->n_rows; i++)
    if (row_matches(sb, i)) return i;
  return -1;
}

/* ---------- callbacks ---------- */
static void on_row_clicked(GtkGestureClick *g, int n, double x, double y,
                           gpointer user) {
  (void)n; (void)x; (void)y;
  GtkWidget *row = gtk_event_controller_get_widget(
      GTK_EVENT_CONTROLLER(g));
  PtSidebar *sb = user;
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "pt-index"));
  g_signal_emit(sb, signals[SIG_SELECTED], 0, idx);
}

static void on_remove_clicked(GtkButton *btn, gpointer user) {
  PtSidebar *sb = user;
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "pt-index"));
  g_signal_emit(sb, signals[SIG_REMOVE], 0, idx);
}

static void on_add_clicked(GtkButton *btn, gpointer user) {
  (void)btn;
  g_signal_emit(PT_SIDEBAR(user), signals[SIG_ADD], 0);
}

/* ---------- row rendering ---------- */
static void rebuild_rows(PtSidebar *sb) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(sb->rows_box)) != NULL)
    gtk_box_remove(GTK_BOX(sb->rows_box), child);

  for (int i = 0; i < sb->n_rows; i++) {
    if (!row_matches(sb, i)) continue;
    const PtSidebarRow *r = &sb->rows[i];

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(row, "pt-project-row");
    /* The row carries no dot any more; the accent survives as the active
     * row's inset left edge (see .pt-project-row.active.pt-aN). */
    pt_accent_set_class(row, r->accent);
    if (i == sb->active) gtk_widget_add_css_class(row, "active");
    /* Original project index — the window never sees filtered positions. */
    g_object_set_data(G_OBJECT(row), "pt-index", GINT_TO_POINTER(i));

    GtkWidget *name = gtk_label_new(r->name);
    gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(name, "pt-name");
    gtk_box_append(GTK_BOX(row), name);

    char *btxt = NULL;
    gboolean dirty = FALSE;
    if (r->missing) {
      btxt = g_strdup("[missing]");
      dirty = TRUE;
    } else if (r->is_repo) {
      btxt = r->changed > 0 ? g_strdup_printf("%s ✚%d", r->branch, r->changed)
                            : g_strdup(r->branch);
      dirty = r->changed > 0;
    }
    if (btxt != NULL) {
      GtkWidget *branch = gtk_label_new(btxt);
      g_free(btxt);
      gtk_label_set_xalign(GTK_LABEL(branch), 0.0f);
      gtk_label_set_ellipsize(GTK_LABEL(branch), PANGO_ELLIPSIZE_END);
      gtk_widget_set_hexpand(branch, FALSE);
      gtk_widget_add_css_class(branch, "pt-branch");
      if (dirty) gtk_widget_add_css_class(branch, "dirty");
      gtk_box_append(GTK_BOX(row), branch);
    }

    GtkWidget *spacer = gtk_label_new(NULL);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(row), spacer);

    char ctxt[32];
    GtkWidget *count;
    if (r->running > 0) {
      g_snprintf(ctxt, sizeof(ctxt), "%d ⏵", r->running);
      count = gtk_label_new(ctxt);
      gtk_widget_add_css_class(count, "pt-run-count");
      pt_accent_set_class(count, r->accent);
    } else {
      g_snprintf(ctxt, sizeof(ctxt), "%d", r->shell_count);
      count = gtk_label_new(ctxt);
      gtk_widget_add_css_class(count, "pt-shell-count");
    }
    gtk_widget_set_halign(count, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(count, GTK_ALIGN_CENTER);

    GtkWidget *rm = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(rm, "flat");
    gtk_widget_add_css_class(rm, "pt-remove");
    gtk_widget_set_halign(rm, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(rm, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(rm), "pt-index", GINT_TO_POINTER(i));
    g_signal_connect(rm, "clicked", G_CALLBACK(on_remove_clicked), sb);

    /* The × sits ON TOP of the count in one end-of-row slot: the count shows
     * at rest, and on row hover the × fades in over it (the count fades out
     * via .pt-project-row:hover — see style.css). No side-by-side gap. */
    GtkWidget *slot = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(slot), count);
    gtk_overlay_add_overlay(GTK_OVERLAY(slot), rm);
    gtk_widget_set_halign(slot, GTK_ALIGN_END);
    gtk_widget_set_valign(slot, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), slot);

    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_row_clicked), sb);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
    gtk_box_append(GTK_BOX(sb->rows_box), row);
  }
}

/* ---------- search ---------- */
static void on_search_changed(GtkEditable *ed, gpointer user) {
  PtSidebar *sb = user;
  g_free(sb->query);
  sb->query = g_strdup(gtk_editable_get_text(ed));
  rebuild_rows(sb);
}

static void on_search_activate(GtkText *txt, gpointer user) {
  (void)txt;
  PtSidebar *sb = user;
  int idx = first_visible_index(sb);
  if (idx >= 0) g_signal_emit(sb, signals[SIG_SELECTED], 0, idx);
}

static gboolean on_search_key(GtkEventControllerKey *ctl, guint keyval,
                              guint keycode, GdkModifierType state,
                              gpointer user) {
  (void)ctl; (void)keycode; (void)state;
  if (keyval != GDK_KEY_Escape) return FALSE;
  PtSidebar *sb = user;
  /* Clearing the entry fires "changed", which resets query and rebuilds. */
  gtk_editable_set_text(GTK_EDITABLE(sb->search), "");
  g_signal_emit(sb, signals[SIG_SEARCH_ESCAPE], 0);
  return TRUE;
}

void pt_sidebar_focus_search(PtSidebar *sb) {
  gtk_widget_grab_focus(sb->search);
}

/* ---------- public API ---------- */
static gboolean row_equal(const PtSidebarRow *a, const PtSidebarRow *b) {
  return a->changed == b->changed && a->is_repo == b->is_repo &&
         a->missing == b->missing && a->accent == b->accent &&
         a->shell_count == b->shell_count && a->running == b->running &&
         g_strcmp0(a->branch, b->branch) == 0 &&
         g_strcmp0(a->name, b->name) == 0 &&
         g_strcmp0(a->path, b->path) == 0;
}

static gboolean same_as_rendered(PtSidebar *sb, const PtSidebarRow *rows,
                                 int n_rows, int active) {
  if (!sb->rendered || n_rows != sb->n_rows || active != sb->active)
    return FALSE;
  for (int i = 0; i < n_rows; i++)
    if (!row_equal(&sb->rows[i], &rows[i])) return FALSE;
  return TRUE;
}

/* Callers refresh on every foreground-command change, which for a chatty pane
 * means a couple of times a second while the underlying data sits still.
 * Rebuilding rows_box that often would destroy buttons mid-click (a widget
 * destroyed between press and release never emits "clicked") and would reset
 * nothing useful, so an unchanged update is a no-op — it also leaves the
 * current filter view exactly as the user left it. */
void pt_sidebar_set_projects(PtSidebar *sb, const PtSidebarRow *rows,
                             int n_rows, int active) {
  if (same_as_rendered(sb, rows, n_rows, active)) return;
  clear_rows(sb);
  if (n_rows > 0) {
    sb->rows = g_new0(PtSidebarRow, n_rows);
    sb->n_rows = n_rows;
    for (int i = 0; i < n_rows; i++) {
      sb->rows[i] = rows[i];
      sb->rows[i].name = g_strdup(rows[i].name);
      sb->rows[i].path = g_strdup(rows[i].path);
    }
  }
  sb->active = active;
  sb->rendered = TRUE;
  rebuild_rows(sb);
}

/* ---------- GObject ---------- */
static void pt_sidebar_dispose(GObject *obj) {
  PtSidebar *sb = PT_SIDEBAR(obj);
  clear_rows(sb);
  g_clear_pointer(&sb->query, g_free);
  g_clear_pointer(&sb->box, gtk_widget_unparent);
  G_OBJECT_CLASS(pt_sidebar_parent_class)->dispose(obj);
}

/* A size request only raises the MINIMUM: GtkBoxLayout would still hand the
 * sidebar its natural width, and an ellipsizing label reports its full text
 * width as natural. A long project name therefore used to widen the rail (531px
 * measured with a 41-char name) and steal that width from the terminal.
 * Reporting minimum == natural == PT_SIDEBAR_WIDTH pins it for good, so this
 * measure/allocate pair replaces GTK_TYPE_BIN_LAYOUT. */
static void pt_sidebar_measure(GtkWidget *widget, GtkOrientation orientation,
                               int for_size, int *minimum, int *natural,
                               int *minimum_baseline, int *natural_baseline) {
  (void)for_size;
  PtSidebar *sb = PT_SIDEBAR(widget);
  *minimum_baseline = *natural_baseline = -1;
  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    *minimum = *natural = PT_SIDEBAR_WIDTH;
    return;
  }
  if (sb->box == NULL) { *minimum = *natural = 0; return; }
  /* Height-for-width: the width is always PT_SIDEBAR_WIDTH. */
  gtk_widget_measure(sb->box, orientation, PT_SIDEBAR_WIDTH,
                     minimum, natural, minimum_baseline, natural_baseline);
}

static void pt_sidebar_size_allocate(GtkWidget *widget, int width, int height,
                                     int baseline) {
  PtSidebar *sb = PT_SIDEBAR(widget);
  if (sb->box != NULL)
    gtk_widget_allocate(sb->box, width, height, baseline, NULL);
}

static void pt_sidebar_class_init(PtSidebarClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_sidebar_dispose;
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  wc->measure = pt_sidebar_measure;
  wc->size_allocate = pt_sidebar_size_allocate;
  signals[SIG_SELECTED] = g_signal_new("project-selected", PT_TYPE_SIDEBAR,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
  signals[SIG_ADD] = g_signal_new("project-add", PT_TYPE_SIDEBAR,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_REMOVE] = g_signal_new("project-remove", PT_TYPE_SIDEBAR,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
  signals[SIG_SEARCH_ESCAPE] = g_signal_new("search-escape", PT_TYPE_SIDEBAR,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void pt_sidebar_init(PtSidebar *sb) {
  gtk_widget_add_css_class(GTK_WIDGET(sb), "pt-sidebar");
  gtk_widget_set_size_request(GTK_WIDGET(sb), PT_SIDEBAR_WIDTH, -1);
  sb->active = -1;
  sb->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent(sb->box, GTK_WIDGET(sb));

  /* search: ⌕ [.................] */
  GtkWidget *search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(search_box, "pt-search");
  GtkWidget *glyph = gtk_label_new("⌕");
  gtk_widget_add_css_class(glyph, "pt-search-glyph");
  gtk_box_append(GTK_BOX(search_box), glyph);
  sb->search = gtk_text_new();
  gtk_text_set_placeholder_text(GTK_TEXT(sb->search), "Search or ^K");
  gtk_widget_set_hexpand(sb->search, TRUE);
  g_signal_connect(sb->search, "changed",
                   G_CALLBACK(on_search_changed), sb);
  g_signal_connect(sb->search, "activate",
                   G_CALLBACK(on_search_activate), sb);
  GtkEventController *keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_search_key), sb);
  gtk_widget_add_controller(sb->search, keys);
  gtk_box_append(GTK_BOX(search_box), sb->search);
  gtk_box_append(GTK_BOX(sb->box), search_box);

  /* Scroll the rows, not the whole rail: the footer button stays put no matter
   * how many projects there are. EXTERNAL horizontally means no h-scrollbar and
   * no horizontal contribution to the sidebar's size request; vertical uses
   * GTK's overlay scrollbar, which fades out when idle. */
  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                 GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroller, TRUE);
  sb->rows_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), sb->rows_box);
  gtk_box_append(GTK_BOX(sb->box), scroller);

  GtkWidget *sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(sep, "pt-sidebar-footer-sep");
  gtk_box_append(GTK_BOX(sb->box), sep);

  GtkWidget *add = gtk_button_new();
  gtk_widget_add_css_class(add, "flat");
  gtk_widget_add_css_class(add, "pt-add-project");
  gtk_widget_set_halign(add, GTK_ALIGN_FILL);
  GtkWidget *add_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *add_label = gtk_label_new("+ Add project");
  gtk_label_set_xalign(GTK_LABEL(add_label), 0.0f);
  gtk_widget_set_hexpand(add_label, TRUE);
  gtk_box_append(GTK_BOX(add_box), add_label);
  GtkWidget *add_hint = gtk_label_new("^N");
  gtk_widget_add_css_class(add_hint, "pt-add-hint");
  gtk_box_append(GTK_BOX(add_box), add_hint);
  gtk_button_set_child(GTK_BUTTON(add), add_box);
  g_signal_connect(add, "clicked", G_CALLBACK(on_add_clicked), sb);
  gtk_box_append(GTK_BOX(sb->box), add);
}

GtkWidget *pt_sidebar_new(void) {
  return g_object_new(PT_TYPE_SIDEBAR, NULL);
}
