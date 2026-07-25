#pragma once
#include <gtk/gtk.h>
#include "pt-term-core.h"

#define PT_TYPE_TERMINAL (pt_terminal_get_type())
G_DECLARE_FINAL_TYPE(PtTerminal, pt_terminal, PT, TERMINAL, GtkWidget)

GtkWidget *pt_terminal_new(const char *cwd);   /* uses the default env below */
/* env_pairs: NULL-terminated "KEY=VALUE" strings for the child. Copied. */
GtkWidget *pt_terminal_new_full(const char *cwd, const char *const *env_pairs);
/* Module-level default env applied by pt_terminal_new (used by pane grids,
 * which create terminals from split leaves without project context).
 * Copied; pass NULL to clear. */
void pt_terminal_set_default_env(const char *const *env_pairs);
PtTermCore *pt_terminal_core(PtTerminal *t);
gboolean pt_terminal_running(PtTerminal *t);   /* fg process other than the shell */
int pt_terminal_last_exit(PtTerminal *t);      /* -1 until the prompt reports */
char *pt_terminal_current_cwd(PtTerminal *t);  /* /proc/<pid>/cwd, caller frees */
void pt_terminal_paste(PtTerminal *t);          /* async clipboard paste */
void pt_terminal_copy(PtTerminal *t);           /* copy selection to clipboard */
const char *pt_terminal_last_command(PtTerminal *t);  /* fg comm; NULL before first poll */
int pt_terminal_font_size(void);            /* shared across all terminals */
void pt_terminal_set_font_size(int pts);    /* clamped; re-measures all panes */
/* GObject signals: "exited" (int), "title-changed" (const char*), "activity" (void),
 *                  "command-changed" (const char*) — fg program of the pane changed */
