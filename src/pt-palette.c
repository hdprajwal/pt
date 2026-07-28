#include "pt-palette.h"
#include "pt-fuzzy.h"
#include "pt-accent.h"

/* The palette never scrolls: it shows the six best matches and nothing else. */
#define PT_PALETTE_ROWS  6
#define PT_PALETTE_WIDTH 620

enum { SIG_ACTIVATED, SIG_CLOSED, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _PtPalette {
  GtkWidget parent_instance;
  GtkWidget *scrim;     /* sole child of the widget; .pt-palette-scrim */
  GtkWidget *panel;     /* .pt-palette */
  GtkWidget *entry;     /* GtkText holding the query */
  GtkWidget *list;      /* vertical box of rows, rebuilt per query */
  PtPaletteItem *items; /* owned; NULL when closed */
  int n_items;
  int shown[PT_PALETTE_ROWS];  /* row -> index into items */
  int n_shown;
  int selected;         /* index into shown[]; -1 when nothing matches */
  gboolean open;
};

G_DEFINE_FINAL_TYPE(PtPalette, pt_palette, GTK_TYPE_WIDGET)

/* ---------- owned items ---------- */
static void free_items(PtPalette *p) {
  for (int i = 0; i < p->n_items; i++) {
    g_free(p->items[i].name);
    g_free(p->items[i].detail);
    g_free(p->items[i].shortcut);
  }
  g_clear_pointer(&p->items, g_free);
  p->n_items = 0;
  p->n_shown = 0;
  p->selected = -1;
}

/* ---------- helpers ---------- */
static void clear_list(PtPalette *p) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(p->list)) != NULL)
    gtk_box_remove(GTK_BOX(p->list), child);
}

/* Highlight without rebuilding: arrow keys must not destroy the rows a click
 * gesture might be sitting on. */
static void apply_selection(PtPalette *p) {
  int i = 0;
  for (GtkWidget *row = gtk_widget_get_first_child(p->list); row != NULL;
       row = gtk_widget_get_next_sibling(row), i++) {
    if (i == p->selected) gtk_widget_add_css_class(row, "selected");
    else gtk_widget_remove_css_class(row, "selected");
  }
}

/* Pick the six best matches for `q`, score descending, ties in natural order.
 * An empty query scores everything 1, so it degenerates to "the first six". */
static void filter_items(PtPalette *p, const char *q) {
  gboolean filtering = q != NULL && q[0] != '\0';
  struct { int idx; int score; } top[PT_PALETTE_ROWS];
  int n = 0;

  for (int i = 0; i < p->n_items; i++) {
    int sn = pt_fuzzy_score(q, p->items[i].name);
    int sd = pt_fuzzy_score(q, p->items[i].detail);
    int s = MAX(sn, sd);
    if (filtering && s == 0) continue;
    /* Full and no better than the worst kept: a tie loses to the earlier item,
     * which is what keeps the sort stable. */
    if (n == PT_PALETTE_ROWS && s <= top[PT_PALETTE_ROWS - 1].score) continue;
    int pos = n < PT_PALETTE_ROWS ? n : PT_PALETTE_ROWS - 1;
    while (pos > 0 && top[pos - 1].score < s) {
      top[pos] = top[pos - 1];
      pos--;
    }
    top[pos].idx = i;
    top[pos].score = s;
    if (n < PT_PALETTE_ROWS) n++;
  }

  for (int i = 0; i < n; i++) p->shown[i] = top[i].idx;
  p->n_shown = n;
}

/* ---------- row rendering ---------- */
static void on_row_pressed(GtkGestureClick *g, int n, double x, double y,
                           gpointer user);

static void rebuild(PtPalette *p) {
  if (!p->open) return;
  filter_items(p, gtk_editable_get_text(GTK_EDITABLE(p->entry)));
  /* The list can shrink under a selection that was valid a keystroke ago. */
  if (p->selected >= p->n_shown) p->selected = p->n_shown - 1;
  if (p->selected < 0 && p->n_shown > 0) p->selected = 0;
  if (p->n_shown == 0) p->selected = -1;

  clear_list(p);
  for (int i = 0; i < p->n_shown; i++) {
    const PtPaletteItem *it = &p->items[p->shown[i]];

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(row, "pt-palette-row");
    if (i == p->selected) gtk_widget_add_css_class(row, "selected");
    g_object_set_data(G_OBJECT(row), "pt-row", GINT_TO_POINTER(i));

    GtkWidget *dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(dot, "pt-dot");
    gtk_widget_add_css_class(dot, "pt-dot-6");
    pt_accent_set_class(dot, it->accent);
    gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), dot);

    GtkWidget *name = gtk_label_new(it->name);
    gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_widget_add_css_class(name, "pt-palette-name");
    gtk_box_append(GTK_BOX(row), name);

    GtkWidget *detail = gtk_label_new(it->detail != NULL ? it->detail : "");
    gtk_label_set_xalign(GTK_LABEL(detail), 1.0f);
    gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_START);
    gtk_widget_add_css_class(detail, "pt-palette-detail");
    gtk_box_append(GTK_BOX(row), detail);

    GtkWidget *kind = gtk_label_new(it->is_command ? "COMMAND"
                                    : it->is_shell ? "SHELL" : "PROJECT");
    gtk_widget_set_valign(kind, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(kind, "pt-palette-kind");
    gtk_box_append(GTK_BOX(row), kind);

    GtkWidget *sc = gtk_label_new(it->shortcut != NULL ? it->shortcut : "");
    gtk_widget_set_valign(sc, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(sc, "pt-palette-shortcut");
    gtk_box_append(GTK_BOX(row), sc);

    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_row_pressed), p);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));

    gtk_box_append(GTK_BOX(p->list), row);
  }
}

/* ---------- activation ---------- */
static void activate_selected(PtPalette *p) {
  if (!p->open) return;
  if (p->selected < 0 || p->selected >= p->n_shown) {
    pt_palette_close(p);
    return;
  }
  /* Copy before emitting: the handler switches projects, and close() below
   * frees the array the item lives in. */
  const PtPaletteItem *it = &p->items[p->shown[p->selected]];
  int project_idx = it->project_idx;
  int tab_idx = it->tab_idx;
  g_signal_emit(p, signals[SIG_ACTIVATED], 0, project_idx, tab_idx);
  pt_palette_close(p);
}

static void on_row_pressed(GtkGestureClick *g, int n, double x, double y,
                           gpointer user) {
  (void)n; (void)x; (void)y;
  PtPalette *p = user;
  GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
  p->selected = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "pt-row"));
  activate_selected(p);
}

/* ---------- input ---------- */
static void on_query_changed(GtkEditable *ed, gpointer user) {
  (void)ed;
  PtPalette *p = user;
  /* Every edit re-ranks the list, so the old highlight means nothing. */
  p->selected = 0;
  rebuild(p);
}

static void move_selection(PtPalette *p, int delta) {
  if (p->n_shown == 0) return;
  int next = p->selected + delta;
  p->selected = CLAMP(next, 0, p->n_shown - 1);
  apply_selection(p);
}

static gboolean on_key(GtkEventControllerKey *ctl, guint keyval, guint keycode,
                       GdkModifierType state, gpointer user) {
  (void)ctl; (void)keycode;
  PtPalette *p = user;
  if (!p->open) return FALSE;

  switch (keyval) {
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
      move_selection(p, 1);
      return TRUE;
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
      move_selection(p, -1);
      return TRUE;
    /* Tab must never fall through: the query entry is the only focusable
     * widget in the palette, so GTK would hand focus to the terminal
     * underneath and every later keystroke would land there instead. */
    case GDK_KEY_Tab:
    case GDK_KEY_KP_Tab:
      move_selection(p, (state & GDK_SHIFT_MASK) != 0 ? -1 : 1);
      return TRUE;
    case GDK_KEY_ISO_Left_Tab:
      move_selection(p, -1);
      return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter:
      activate_selected(p);
      return TRUE;
    case GDK_KEY_Escape: {
      const char *q = gtk_editable_get_text(GTK_EDITABLE(p->entry));
      /* First Escape empties a non-empty query; the next one closes. */
      if (q != NULL && q[0] != '\0')
        gtk_editable_set_text(GTK_EDITABLE(p->entry), "");  /* fires changed */
      else
        pt_palette_close(p);
      return TRUE;
    }
    default:
      return FALSE;
  }
}

/* Anything outside the panel dismisses. */
static void on_scrim_pressed(GtkGestureClick *g, int n, double x, double y,
                             gpointer user) {
  (void)g; (void)n;
  PtPalette *p = user;
  if (!p->open) return;
  GtkWidget *hit = gtk_widget_pick(p->scrim, x, y, GTK_PICK_DEFAULT);
  for (GtkWidget *a = hit; a != NULL; a = gtk_widget_get_parent(a))
    if (a == p->panel) return;
  pt_palette_close(p);
}

/* ---------- public API ---------- */
void pt_palette_open(PtPalette *p, PtPaletteItem *items, int n_items) {
  g_return_if_fail(PT_IS_PALETTE(p));
  free_items(p);
  p->items = items;
  /* A projectless window hands over a NULL array; the palette opens empty. */
  p->n_items = (items != NULL && n_items > 0) ? n_items : 0;
  p->selected = 0;
  p->open = TRUE;

  gtk_widget_set_visible(GTK_WIDGET(p), TRUE);
  gtk_widget_set_can_target(GTK_WIDGET(p), TRUE);
  /* Setting the text fires "changed" only when it actually changes, so rebuild
   * unconditionally afterwards. */
  gtk_editable_set_text(GTK_EDITABLE(p->entry), "");
  rebuild(p);
  gtk_widget_grab_focus(p->entry);
}

void pt_palette_close(PtPalette *p) {
  g_return_if_fail(PT_IS_PALETTE(p));
  if (!p->open) return;
  p->open = FALSE;
  gtk_widget_set_visible(GTK_WIDGET(p), FALSE);
  /* Belt and braces: an invisible widget is not picked, but this also keeps the
   * overlay from swallowing clicks meant for the terminal underneath. */
  gtk_widget_set_can_target(GTK_WIDGET(p), FALSE);
  clear_list(p);
  free_items(p);
  g_signal_emit(p, signals[SIG_CLOSED], 0);
}

gboolean pt_palette_is_open(PtPalette *p) {
  g_return_val_if_fail(PT_IS_PALETTE(p), FALSE);
  return p->open;
}

/* ---------- GObject ---------- */
/* No "closed" here on purpose: the window is on its way out too, and its
 * handler would reach for panes that dispose has already dropped. */
static void pt_palette_dispose(GObject *obj) {
  PtPalette *p = PT_PALETTE(obj);
  p->open = FALSE;
  free_items(p);
  g_clear_pointer(&p->scrim, gtk_widget_unparent);
  p->panel = p->entry = p->list = NULL;
  G_OBJECT_CLASS(pt_palette_parent_class)->dispose(obj);
}

static void pt_palette_class_init(PtPaletteClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_palette_dispose;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass),
                                           GTK_TYPE_BIN_LAYOUT);
  signals[SIG_ACTIVATED] = g_signal_new("activated", PT_TYPE_PALETTE,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2,
      G_TYPE_INT, G_TYPE_INT);
  signals[SIG_CLOSED] = g_signal_new("closed", PT_TYPE_PALETTE,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void pt_palette_init(PtPalette *p) {
  p->selected = -1;
  gtk_widget_set_visible(GTK_WIDGET(p), FALSE);
  gtk_widget_set_can_target(GTK_WIDGET(p), FALSE);

  p->scrim = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(p->scrim, "pt-palette-scrim");
  gtk_widget_set_hexpand(p->scrim, TRUE);
  gtk_widget_set_vexpand(p->scrim, TRUE);
  gtk_widget_set_parent(p->scrim, GTK_WIDGET(p));

  p->panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(p->panel, "pt-palette");
  gtk_widget_set_halign(p->panel, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(p->panel, GTK_ALIGN_START);
  gtk_widget_set_margin_top(p->panel, 90);
  gtk_widget_set_size_request(p->panel, PT_PALETTE_WIDTH, -1);
  gtk_box_append(GTK_BOX(p->scrim), p->panel);

  GtkWidget *query = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(query, "pt-palette-query");
  GtkWidget *glyph = gtk_label_new("⌕");
  gtk_widget_add_css_class(glyph, "pt-search-glyph");
  gtk_box_append(GTK_BOX(query), glyph);
  p->entry = gtk_text_new();
  gtk_widget_set_hexpand(p->entry, TRUE);
  g_signal_connect(p->entry, "changed", G_CALLBACK(on_query_changed), p);
  gtk_box_append(GTK_BOX(query), p->entry);
  GtkWidget *scope = gtk_label_new("projects · shells");
  gtk_widget_set_valign(scope, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(scope, "pt-palette-scope");
  gtk_box_append(GTK_BOX(query), scope);
  gtk_box_append(GTK_BOX(p->panel), query);

  p->list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(p->panel), p->list);

  /* CAPTURE: the query entry is focused, so Up/Down/Enter/Escape must be
   * intercepted on the way down or GtkText eats them first. */
  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), p);
  gtk_widget_add_controller(GTK_WIDGET(p), keys);

  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click, "pressed", G_CALLBACK(on_scrim_pressed), p);
  gtk_widget_add_controller(p->scrim, GTK_EVENT_CONTROLLER(click));
}

GtkWidget *pt_palette_new(void) {
  return g_object_new(PT_TYPE_PALETTE, NULL);
}
