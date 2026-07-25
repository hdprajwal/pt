#pragma once

#include <gtk/gtk.h>
#include "pt-session.h"   /* PT_ACCENT_COUNT */

/* Exactly one pt-aN class survives on `wdg`. Accents outside 0..5 are folded
 * back into range rather than dropped, so a bad index still colours something.
 * Removing every class first makes this safe on a widget that is recoloured in
 * place; on a freshly built widget the removals are no-ops. */
static inline void pt_accent_set_class(GtkWidget *wdg, int accent) {
  int a = accent % PT_ACCENT_COUNT;
  if (a < 0) a += PT_ACCENT_COUNT;
  for (int i = 0; i < PT_ACCENT_COUNT; i++) {
    char c[8];
    g_snprintf(c, sizeof(c), "pt-a%d", i);
    gtk_widget_remove_css_class(wdg, c);
  }
  char c[8];
  g_snprintf(c, sizeof(c), "pt-a%d", a);
  gtk_widget_add_css_class(wdg, c);
}
