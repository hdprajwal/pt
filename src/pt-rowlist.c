#include "pt-rowlist.h"

/* The row's item index, in the same spelling the hand-built rows use. */
#define PT_ROWLIST_INDEX_KEY "pt-index"

enum { SIG_ROW_ACTIVATED, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _PtRowList {
  GObject parent_instance;
  GtkBox *host;            /* weak: the host widget owns us, and clears this */
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
  /* No block to index into: build_row would be handed NULL n_items times. */
  g_return_if_fail(items != NULL || n_items == 0);
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

  /* Adopt the new block, take the old rows down, and only then release the old
   * block — in that order. A row is free to have kept a pointer into the items
   * it was built from (an in-flight gesture, a weak-ref teardown), and it reads
   * that as it goes down, so the block it came from has to outlive it.
   *
   * The release is unconditional, the same block handed back included: every
   * call transfers ownership, so a refcounted array arrives with a fresh
   * reference and the one held here has to go back or it leaks. Stashing the old
   * pair first is what makes that case safe — the new reference is already held
   * by the time the old one is dropped. */
  gpointer old_items = rl->items;
  GDestroyNotify old_free = rl->items_free;
  rl->items = items;
  rl->n_items = n_items;
  rl->items_free = items_free;
  rl->rendered = TRUE;

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(rl->host))) != NULL)
    gtk_box_remove(rl->host, child);

  if (old_items != NULL && old_free != NULL) old_free(old_items);

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
  /* A live host means the list is being dropped first, so take the rows down
   * here — same ordering as a rebuild, rows before the block they were built
   * from. A host that is already gone cleared the weak pointer, and its rows
   * went with it. */
  if (rl->host != NULL) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(rl->host))) != NULL)
      gtk_box_remove(rl->host, child);
    g_object_remove_weak_pointer(G_OBJECT(rl->host), (gpointer *)&rl->host);
    rl->host = NULL;
  }
  free_items(rl);
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
  /* Weak, not a reference: the host owns the list, so a reference would be a
   * cycle. It also makes "the host is gone" something the code can actually
   * see — a set arriving after the host died then renders nothing instead of
   * reaching into a freed box. */
  g_object_add_weak_pointer(G_OBJECT(host), (gpointer *)&rl->host);
  return rl;
}
