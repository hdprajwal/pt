#include "pt-rowlist.h"

/* The row's item index, in the same spelling the hand-built rows use. */
#define PT_ROWLIST_INDEX_KEY "pt-index"

enum { SIG_ROW_ACTIVATED, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _PtRowList {
  GObject parent_instance;
  GtkBox *host;            /* not a reference: the host widget owns us */
  gpointer items;          /* the block the rows on screen were built from */
  guint n_items;
  GDestroyNotify items_free;
  gboolean rendered;       /* the host reflects `items` */
};

G_DEFINE_FINAL_TYPE(PtRowList, pt_rowlist, G_TYPE_OBJECT)

static void free_items(PtRowList *rl) {
  if (rl->items != NULL && rl->items_free != NULL) rl->items_free(rl->items);
  rl->items = NULL;
  rl->n_items = 0;
  rl->items_free = NULL;
}

static void on_row_pressed(GtkGestureClick *g, int n, double x, double y,
                           gpointer user) {
  (void)n; (void)x; (void)y;
  GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
  g_signal_emit(PT_ROWLIST(user), signals[SIG_ROW_ACTIVATED], 0,
                pt_rowlist_row_index(row));
}

/* ---------- public API ---------- */
int pt_rowlist_row_index(GtkWidget *row) {
  g_return_val_if_fail(GTK_IS_WIDGET(row), -1);
  return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row),
                                           PT_ROWLIST_INDEX_KEY));
}

void pt_rowlist_mark_selected(GtkWidget *row_host, int sel) {
  g_return_if_fail(GTK_IS_WIDGET(row_host));
  int i = 0;
  for (GtkWidget *row = gtk_widget_get_first_child(row_host); row != NULL;
       row = gtk_widget_get_next_sibling(row), i++) {
    if (i == sel) gtk_widget_add_css_class(row, "selected");
    else gtk_widget_remove_css_class(row, "selected");
  }
}

void pt_rowlist_set(PtRowList *rl, gpointer items, guint n_items,
                    GtkWidget *(*build_row)(gpointer item, guint idx,
                                            gpointer u),
                    gboolean (*items_equal)(gpointer a, guint na, gpointer b,
                                            guint nb, gpointer u),
                    gpointer u, GDestroyNotify items_free) {
  g_return_if_fail(PT_IS_ROWLIST(rl));
  g_return_if_fail(build_row != NULL);
  /* The host widget already took its rows down (its dispose ran), so there is
   * nothing to render — but the block was handed over, so drop it. */
  if (rl->host == NULL) {
    if (items != NULL && items_free != NULL) items_free(items);
    return;
  }

  if (rl->rendered && items_equal != NULL &&
      items_equal(rl->items, rl->n_items, items, n_items, u)) {
    /* Nothing on screen would change, so leave the rows — and the gesture a
     * press may be sitting on — alone. The block was handed over all the same,
     * so free it now; for the very block already held that is just the caller's
     * extra reference going back. */
    if (items != NULL && items_free != NULL) items_free(items);
    return;
  }

  if (rl->items != items) free_items(rl);
  rl->items = items;
  rl->n_items = n_items;
  rl->items_free = items_free;
  rl->rendered = TRUE;

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(rl->host))) != NULL)
    gtk_box_remove(rl->host, child);

  /* One gesture per row, but only when someone is listening: an inert list
   * (the info panel's files) must not start claiming presses. */
  gboolean clickable = g_signal_has_handler_pending(
      rl, signals[SIG_ROW_ACTIVATED], 0, FALSE);

  for (guint i = 0; i < n_items; i++) {
    GtkWidget *row = build_row(items, i, u);
    if (row == NULL) continue;
    g_object_set_data(G_OBJECT(row), PT_ROWLIST_INDEX_KEY,
                      GINT_TO_POINTER((int)i));
    if (clickable) {
      GtkGesture *click = gtk_gesture_click_new();
      g_signal_connect(click, "pressed", G_CALLBACK(on_row_pressed), rl);
      gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
    }
    gtk_box_append(rl->host, row);
  }
}

/* ---------- GObject ---------- */
static void pt_rowlist_dispose(GObject *obj) {
  PtRowList *rl = PT_ROWLIST(obj);
  free_items(rl);
  rl->host = NULL;   /* the host widget takes its own children down */
  G_OBJECT_CLASS(pt_rowlist_parent_class)->dispose(obj);
}

static void pt_rowlist_class_init(PtRowListClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_rowlist_dispose;
  signals[SIG_ROW_ACTIVATED] = g_signal_new("row-activated", PT_TYPE_ROWLIST,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void pt_rowlist_init(PtRowList *rl) { (void)rl; }

PtRowList *pt_rowlist_new(GtkBox *host) {
  g_return_val_if_fail(GTK_IS_BOX(host), NULL);
  PtRowList *rl = g_object_new(PT_TYPE_ROWLIST, NULL);
  rl->host = host;
  return rl;
}
