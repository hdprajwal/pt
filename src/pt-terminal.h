#pragma once
#include <gtk/gtk.h>
#include "pt-term-core.h"

#define PT_TYPE_TERMINAL (pt_terminal_get_type())
G_DECLARE_FINAL_TYPE(PtTerminal, pt_terminal, PT, TERMINAL, GtkWidget)

GtkWidget *pt_terminal_new(const char *cwd);
PtTermCore *pt_terminal_core(PtTerminal *t);
char *pt_terminal_current_cwd(PtTerminal *t);  /* /proc/<pid>/cwd, caller frees */
void pt_terminal_paste(PtTerminal *t);          /* async clipboard paste */
void pt_terminal_copy(PtTerminal *t);           /* copy selection to clipboard */
const char *pt_terminal_last_command(PtTerminal *t);  /* fg comm; NULL before first poll */
/* GObject signals: "exited" (int), "title-changed" (const char*), "activity" (void),
 *                  "command-changed" (const char*) — fg program of the pane changed */
