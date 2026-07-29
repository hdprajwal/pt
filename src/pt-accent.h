#pragma once

#include <gtk/gtk.h>
#include "pt-session.h"   /* PT_ACCENT_COUNT */

/* The CSS class per accent, spelled once. Indexed by accent, so the order is
 * the definition of which class an accent means. */
static const char *const pt_accent_classes[] = {
  "pt-a0", "pt-a1", "pt-a2", "pt-a3", "pt-a4", "pt-a5",
};
G_STATIC_ASSERT(G_N_ELEMENTS(pt_accent_classes) == PT_ACCENT_COUNT);

/* Exactly one pt-aN class survives on `wdg`. Accents outside 0..5 are folded
 * back into range rather than dropped, so a bad index still colours something.
 * Removing the others first makes this safe on a widget that is recoloured in
 * place; on a freshly built widget the removals are no-ops.
 * The already-right case returns immediately: this is called on every row of
 * every sidebar rebuild, and GTK invalidates style on each remove_css_class
 * whether or not the class was there. */
static inline void pt_accent_set_class(GtkWidget *wdg, int accent) {
  int a = accent % PT_ACCENT_COUNT;
  if (a < 0) a += PT_ACCENT_COUNT;
  if (gtk_widget_has_css_class(wdg, pt_accent_classes[a])) return;
  for (int i = 0; i < PT_ACCENT_COUNT; i++)
    if (i != a) gtk_widget_remove_css_class(wdg, pt_accent_classes[i]);
  gtk_widget_add_css_class(wdg, pt_accent_classes[a]);
}
