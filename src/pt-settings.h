#pragma once
#include <gtk/gtk.h>
#include "pt-config.h"

#define PT_TYPE_SETTINGS (pt_settings_get_type())
G_DECLARE_FINAL_TYPE(PtSettings, pt_settings, PT, SETTINGS, GtkWidget)

GtkWidget *pt_settings_new(void);

/* Deep-copies `current`. `themes` is a NULL-terminated name list (copied);
 * NULL or empty simply pins the theme row. */
void pt_settings_open(PtSettings *s, const PtConfig *current,
                      const char *const *themes);
void pt_settings_close(PtSettings *s);
gboolean pt_settings_is_open(PtSettings *s);

/* The candidate config as currently shown (owned by the widget, NULL before the
 * first open). It outlives "committed"/"reverted"/"closed", so a handler may
 * still read it while the dialog is going away. */
const PtConfig *pt_settings_config(PtSettings *s);

/* Signals:
 *  "changed"   () — a value moved; window should apply pt_settings_config()
 *  "committed" () — Enter: persist the candidate
 *  "reverted"  () — Esc/scrim: window should re-apply its own config
 *  "closed"    () — emitted on any dismissal, after "committed"/"reverted"
 */
