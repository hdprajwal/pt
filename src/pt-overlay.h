/* pt-overlay.h */
#pragma once
#include <gtk/gtk.h>

/* The scrim-and-panel machinery the command palette and the settings dialog
 * both need: a full-window scrim, a centred panel near the top, keys taken in
 * the CAPTURE phase (with Tab trapped so focus can never escape to a terminal
 * underneath), a click outside the panel that dismisses, and open/close state
 * that hides the host widget and stops it swallowing clicks.
 *
 * The overlay is a plain GObject, owned by the widget that hosts it: that
 * widget passes itself as `host_overlay`, keeps the returned reference, and
 * drops it in dispose. Only the rows, the ranking and the value editing stay in
 * the widget. */
#define PT_TYPE_OVERLAY (pt_overlay_get_type())
G_DECLARE_FINAL_TYPE(PtOverlay, pt_overlay, PT, OVERLAY, GObject)

/* `host_overlay` is the widget the scrim is parented into — a GTK_TYPE_BIN_LAYOUT
 * widget sitting in the window's GtkOverlay. `panel_css_class` styles the panel
 * (".pt-palette", ".pt-settings"); the scrim always carries
 * ".pt-palette-scrim". The host starts hidden and untargetable. */
PtOverlay *pt_overlay_new(GtkWidget *host_overlay, const char *panel_css_class);

/* The content container: append the query row, the row list, the hint. Its
 * width is the host's business (gtk_widget_set_size_request). */
GtkBox *pt_overlay_panel(PtOverlay *o);

void pt_overlay_open(PtOverlay *o);
/* Hides the host and emits "closed"; a no-op when already closed. Dispose does
 * NOT emit it — by then the window is on its way out too, and its handler would
 * reach for state that has already been dropped. */
void pt_overlay_close(PtOverlay *o);
gboolean pt_overlay_is_open(PtOverlay *o);

/* Keys arrive here in the CAPTURE phase while the overlay is open, before any
 * focused child (a GtkText) gets them. Return TRUE when handled. Tab,
 * KP_Tab and ISO_Left_Tab never fall through even when the handler ignores
 * them: nothing else in an overlay is focusable, so GTK would hand focus to the
 * terminal underneath and every later keystroke would land there. */
void pt_overlay_set_key_handler(PtOverlay *o,
                                gboolean (*fn)(guint keyval, GdkModifierType st,
                                               gpointer u),
                                gpointer u);

/* Signals:
 * - "closed": pt_overlay_close() ran. Free what was on screen here.
 * - "dismissed": a press landed outside the panel. What that means is the
 *   host's call — the palette closes, the settings dialog cancels first — so
 *   the overlay reports it instead of closing itself. */
