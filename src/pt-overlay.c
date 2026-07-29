#include "pt-overlay.h"

/* Far enough down that the panel reads as floating over the content rather than
 * hanging off the title bar. The same distance for every overlay, so they land
 * in the same place. */
#define PT_OVERLAY_MARGIN_TOP 90

enum { SIG_CLOSED, SIG_DISMISSED, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _PtOverlay {
  GObject parent_instance;
  /* Not a reference: the host owns the overlay, so a reference here would be a
   * cycle. It outlives us by construction. */
  GtkWidget *host;
  GtkWidget *scrim;   /* sole child of the host; .pt-palette-scrim */
  GtkWidget *panel;   /* the centred content box */
  gboolean (*key_fn)(guint keyval, GdkModifierType st, gpointer u);
  gpointer key_user;
  gboolean open;
};

G_DEFINE_FINAL_TYPE(PtOverlay, pt_overlay, G_TYPE_OBJECT)

/* ---------- input ---------- */
static gboolean on_key(GtkEventControllerKey *ctl, guint keyval, guint keycode,
                       GdkModifierType state, gpointer user) {
  (void)ctl; (void)keycode;
  PtOverlay *o = user;
  if (!o->open) return FALSE;
  if (o->key_fn != NULL && o->key_fn(keyval, state, o->key_user)) return TRUE;
  /* Trapped even unhandled: see pt_overlay_set_key_handler. */
  switch (keyval) {
    case GDK_KEY_Tab:
    case GDK_KEY_KP_Tab:
    case GDK_KEY_ISO_Left_Tab:
      return TRUE;
    default:
      return FALSE;
  }
}

/* Anything outside the panel dismisses. */
static void on_scrim_pressed(GtkGestureClick *g, int n, double x, double y,
                             gpointer user) {
  (void)g; (void)n;
  PtOverlay *o = user;
  if (!o->open) return;
  GtkWidget *hit = gtk_widget_pick(o->scrim, x, y, GTK_PICK_DEFAULT);
  for (GtkWidget *a = hit; a != NULL; a = gtk_widget_get_parent(a))
    if (a == o->panel) return;
  g_signal_emit(o, signals[SIG_DISMISSED], 0);
}

/* ---------- public API ---------- */
GtkBox *pt_overlay_panel(PtOverlay *o) {
  g_return_val_if_fail(PT_IS_OVERLAY(o), NULL);
  return GTK_BOX(o->panel);
}

void pt_overlay_open(PtOverlay *o) {
  g_return_if_fail(PT_IS_OVERLAY(o));
  o->open = TRUE;
  gtk_widget_set_visible(o->host, TRUE);
  gtk_widget_set_can_target(o->host, TRUE);
}

void pt_overlay_close(PtOverlay *o) {
  g_return_if_fail(PT_IS_OVERLAY(o));
  if (!o->open) return;
  o->open = FALSE;
  gtk_widget_set_visible(o->host, FALSE);
  /* Belt and braces: an invisible widget is not picked, but this also keeps the
   * overlay from swallowing clicks meant for the terminal underneath. */
  gtk_widget_set_can_target(o->host, FALSE);
  g_signal_emit(o, signals[SIG_CLOSED], 0);
}

gboolean pt_overlay_is_open(PtOverlay *o) {
  g_return_val_if_fail(PT_IS_OVERLAY(o), FALSE);
  return o->open;
}

void pt_overlay_set_key_handler(PtOverlay *o,
                                gboolean (*fn)(guint keyval, GdkModifierType st,
                                               gpointer u),
                                gpointer u) {
  g_return_if_fail(PT_IS_OVERLAY(o));
  o->key_fn = fn;
  o->key_user = u;
}

/* ---------- GObject ---------- */
/* No "closed" here on purpose: the host is on its way out too, and its handler
 * would reach for state that dispose has already dropped. */
static void pt_overlay_dispose(GObject *obj) {
  PtOverlay *o = PT_OVERLAY(obj);
  o->open = FALSE;
  o->key_fn = NULL;
  g_clear_pointer(&o->scrim, gtk_widget_unparent);
  o->panel = NULL;
  o->host = NULL;
  G_OBJECT_CLASS(pt_overlay_parent_class)->dispose(obj);
}

static void pt_overlay_class_init(PtOverlayClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_overlay_dispose;
  signals[SIG_CLOSED] = g_signal_new("closed", PT_TYPE_OVERLAY,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_DISMISSED] = g_signal_new("dismissed", PT_TYPE_OVERLAY,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void pt_overlay_init(PtOverlay *o) { (void)o; }

PtOverlay *pt_overlay_new(GtkWidget *host_overlay, const char *panel_css_class) {
  g_return_val_if_fail(GTK_IS_WIDGET(host_overlay), NULL);
  PtOverlay *o = g_object_new(PT_TYPE_OVERLAY, NULL);
  o->host = host_overlay;
  gtk_widget_set_visible(o->host, FALSE);
  gtk_widget_set_can_target(o->host, FALSE);

  o->scrim = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(o->scrim, "pt-palette-scrim");
  gtk_widget_set_hexpand(o->scrim, TRUE);
  gtk_widget_set_vexpand(o->scrim, TRUE);
  gtk_widget_set_parent(o->scrim, o->host);

  o->panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  if (panel_css_class != NULL)
    gtk_widget_add_css_class(o->panel, panel_css_class);
  gtk_widget_set_halign(o->panel, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(o->panel, GTK_ALIGN_START);
  gtk_widget_set_margin_top(o->panel, PT_OVERLAY_MARGIN_TOP);
  gtk_box_append(GTK_BOX(o->scrim), o->panel);

  /* CAPTURE: a focused child (the palette's query entry) would otherwise eat
   * the arrows, Enter and Escape before this ever ran. connect_object so a key
   * or a press arriving while the host tears down finds nothing to call. */
  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect_object(keys, "key-pressed", G_CALLBACK(on_key), o, 0);
  gtk_widget_add_controller(o->host, keys);

  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect_object(click, "pressed", G_CALLBACK(on_scrim_pressed), o, 0);
  gtk_widget_add_controller(o->scrim, GTK_EVENT_CONTROLLER(click));
  return o;
}
