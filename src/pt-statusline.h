/* pt-statusline.h */
#pragma once
#include <gtk/gtk.h>
#include "pt-status-parse.h"

#define PT_TYPE_STATUSLINE (pt_statusline_get_type())
G_DECLARE_FINAL_TYPE(PtStatusline, pt_statusline, PT, STATUSLINE, GtkWidget)

GtkWidget *pt_statusline_new(void);

/* Run state: running TRUE → "● running"; otherwise last_exit > 0 → "✗ exit N",
 * last_exit == 0 → "✓ exit 0", last_exit < 0 (never reported) → plain "✓".
 * progress NULL → the track and the task label are hidden outright; there is no
 * indeterminate mode, because a bar that moves without data is a lie.
 * task_label is the foreground command (e.g. "cargo"); may be NULL.
 * accent indexes the project accent cycle and colours the fill.
 * zoomed shows the pane-zoom chip next to the run state. */
void pt_statusline_update(PtStatusline *sl, gboolean running, int last_exit,
                          const PtProgress *progress, const char *task_label,
                          int accent, gboolean zoomed);
