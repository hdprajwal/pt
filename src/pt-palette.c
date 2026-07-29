#include "pt-palette.h"
#include "pt-fuzzy.h"
#include "pt-accent.h"
#include "pt-overlay.h"
#include "pt-rowlist.h"

/* The palette never scrolls: it shows the six best matches and nothing else. */
#define PT_PALETTE_ROWS  6
#define PT_PALETTE_WIDTH 620

enum { SIG_ACTIVATED, SIG_CLOSED, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _PtPalette {
  GtkWidget parent_instance;
  PtOverlay *overlay;   /* scrim, panel, keys, dismiss, open/closed state */
  GtkWidget *entry;     /* GtkText holding the query */
  GtkWidget *list;      /* box the rows live in */
  PtRowList *rows;      /* rebuilt per query */
  PtPaletteItem *items; /* owned; NULL when closed */
  int n_items;
  int *scores;          /* one per item, refilled per query; sized with items */
  int shown[PT_PALETTE_ROWS];  /* row -> index into items */
  int n_shown;
  int selected;         /* index into shown[]; -1 when nothing matches */
};

G_DEFINE_FINAL_TYPE(PtPalette, pt_palette, GTK_TYPE_WIDGET)

static gboolean is_open(PtPalette *p) {
  return p->overlay != NULL && pt_overlay_is_open(p->overlay);
}

/* ---------- owned items ---------- */
static void free_items(PtPalette *p) {
  for (int i = 0; i < p->n_items; i++) {
    g_free(p->items[i].name);
    g_free(p->items[i].detail);
    g_free(p->items[i].shortcut);
  }
  g_clear_pointer(&p->items, g_free);
  g_clear_pointer(&p->scores, g_free);
  p->n_items = 0;
  p->n_shown = 0;
  p->selected = -1;
}

/* ---------- helpers ---------- */
/* Pick the six best matches for `q`, score descending, ties in natural order.
 * An empty query scores everything 1, so it degenerates to "the first six". */
static void filter_items(PtPalette *p, const char *q) {
  /* An item is worth what its better half matches: typing a path fragment has
   * to find a project by its detail line, not only by its name. The stable
   * top-N selection over those scores is pt_fuzzy_rank_scored's job. */
  const char *needle = q != NULL ? q : "";
  for (int i = 0; i < p->n_items; i++)
    p->scores[i] = MAX(pt_fuzzy_score(needle, p->items[i].name),
                       pt_fuzzy_score(needle, p->items[i].detail));
  p->n_shown = pt_fuzzy_rank_scored(p->scores, p->n_items, p->shown,
                                    PT_PALETTE_ROWS);
}

/* ---------- row rendering ---------- */
/* One row per *shown* position, so the index a click reports indexes shown[]
 * — the same space `selected` lives in. */
static GtkWidget *build_row(gpointer items, guint idx, gpointer user) {
  PtPalette *p = user;
  const PtPaletteItem *it = &((const PtPaletteItem *)items)[p->shown[idx]];

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(row, "pt-palette-row");
  if ((int)idx == p->selected) gtk_widget_add_css_class(row, "selected");

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
  return row;
}

/* The rows are cheap and every keystroke re-ranks them, so there is nothing to
 * dedupe against: no items_equal, and the items block stays the palette's (the
 * row list borrows it, and clear_rows below runs before it is freed). */
static void rebuild(PtPalette *p) {
  if (!is_open(p)) return;
  filter_items(p, gtk_editable_get_text(GTK_EDITABLE(p->entry)));
  /* The list can shrink under a selection that was valid a keystroke ago. */
  if (p->selected >= p->n_shown) p->selected = p->n_shown - 1;
  if (p->selected < 0 && p->n_shown > 0) p->selected = 0;
  if (p->n_shown == 0) p->selected = -1;

  pt_rowlist_set(p->rows, p->items, (guint)p->n_shown, build_row, NULL, p, NULL);
}

static void clear_rows(PtPalette *p) {
  pt_rowlist_set(p->rows, NULL, 0, build_row, NULL, p, NULL);
}

/* ---------- activation ---------- */
static void activate_selected(PtPalette *p) {
  if (!is_open(p)) return;
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

static void on_row_activated(PtRowList *rl, int idx, gpointer user) {
  (void)rl;
  PtPalette *p = user;
  p->selected = idx;
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
  /* Highlight without rebuilding: arrow keys must not destroy the rows a click
   * gesture might be sitting on. */
  pt_rowlist_mark_selected(p->list, p->selected);
}

/* Runs in the overlay's CAPTURE phase, and only while the palette is open. */
static gboolean on_key(guint keyval, GdkModifierType state, gpointer user) {
  PtPalette *p = user;
  switch (keyval) {
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
      move_selection(p, 1);
      return TRUE;
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
      move_selection(p, -1);
      return TRUE;
    /* Tab steps the selection rather than moving focus; the overlay traps it
     * either way, so it can never reach the terminal underneath. */
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

/* A press outside the panel closes, query or no query. */
static void on_dismissed(PtOverlay *o, gpointer user) {
  (void)o;
  pt_palette_close(PT_PALETTE(user));
}

/* The overlay is already hidden by here; what is left is what it was showing. */
static void on_overlay_closed(PtOverlay *o, gpointer user) {
  (void)o;
  PtPalette *p = PT_PALETTE(user);
  clear_rows(p);
  free_items(p);
  g_signal_emit(p, signals[SIG_CLOSED], 0);
}

/* ---------- public API ---------- */
void pt_palette_open(PtPalette *p, PtPaletteItem *items, int n_items) {
  g_return_if_fail(PT_IS_PALETTE(p));
  clear_rows(p);   /* the rows borrow the items free_items is about to drop */
  free_items(p);
  p->items = items;
  /* A projectless window hands over a NULL array; the palette opens empty. */
  p->n_items = (items != NULL && n_items > 0) ? n_items : 0;
  /* Scored once per keystroke, so the room for the scores is taken here and
   * not on the typing path. */
  p->scores = p->n_items > 0 ? g_new0(int, (gsize)p->n_items) : NULL;
  p->selected = 0;
  pt_overlay_open(p->overlay);

  /* Setting the text fires "changed" only when it actually changes, so rebuild
   * unconditionally afterwards. */
  gtk_editable_set_text(GTK_EDITABLE(p->entry), "");
  rebuild(p);
  gtk_widget_grab_focus(p->entry);
}

void pt_palette_close(PtPalette *p) {
  g_return_if_fail(PT_IS_PALETTE(p));
  if (p->overlay != NULL) pt_overlay_close(p->overlay);
}

gboolean pt_palette_is_open(PtPalette *p) {
  g_return_val_if_fail(PT_IS_PALETTE(p), FALSE);
  return is_open(p);
}

/* ---------- GObject ---------- */
/* No "closed" here on purpose: the window is on its way out too, and its
 * handler would reach for panes that dispose has already dropped. Dropping the
 * overlay is what takes the scrim down, and it emits nothing either. */
static void pt_palette_dispose(GObject *obj) {
  PtPalette *p = PT_PALETTE(obj);
  /* Overlay first: it takes the rows down, and a row must never outlive the row
   * list its gesture points at. */
  g_clear_object(&p->overlay);
  g_clear_object(&p->rows);
  free_items(p);
  p->entry = p->list = NULL;
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

  p->overlay = pt_overlay_new(GTK_WIDGET(p), "pt-palette");
  GtkBox *panel = pt_overlay_panel(p->overlay);
  gtk_widget_set_size_request(GTK_WIDGET(panel), PT_PALETTE_WIDTH, -1);
  pt_overlay_set_key_handler(p->overlay, on_key, p);
  g_signal_connect(p->overlay, "dismissed", G_CALLBACK(on_dismissed), p);
  g_signal_connect(p->overlay, "closed", G_CALLBACK(on_overlay_closed), p);

  GtkWidget *query = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(query, "pt-palette-query");
  GtkWidget *glyph = gtk_label_new("⌕");
  gtk_widget_add_css_class(glyph, "pt-search-glyph");
  gtk_box_append(GTK_BOX(query), glyph);
  p->entry = gtk_text_new();
  gtk_widget_set_hexpand(p->entry, TRUE);
  g_signal_connect(p->entry, "changed", G_CALLBACK(on_query_changed), p);
  gtk_box_append(GTK_BOX(query), p->entry);
  GtkWidget *scope = gtk_label_new("projects · shells · commands");
  gtk_widget_set_valign(scope, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(scope, "pt-palette-scope");
  gtk_box_append(GTK_BOX(query), scope);
  gtk_box_append(panel, query);

  p->list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(panel, p->list);
  p->rows = pt_rowlist_new(GTK_BOX(p->list));
  /* Connected before the first rebuild: that is what puts a click gesture on
   * the rows. */
  g_signal_connect(p->rows, "row-activated", G_CALLBACK(on_row_activated), p);
}

GtkWidget *pt_palette_new(void) {
  return g_object_new(PT_TYPE_PALETTE, NULL);
}
