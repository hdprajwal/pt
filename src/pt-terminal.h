#pragma once
#include <gtk/gtk.h>
#include "pt-config.h"
#include "pt-term-core.h"
#include "pt-theme.h"

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
/* Stable for the life of the pane and never reused. A desktop notification
 * sits on screen until the user clicks it, long after the pane that raised it
 * may have gone, so what it carries back is this rather than a pointer. */
guint64 pt_terminal_id(PtTerminal *t);
gboolean pt_terminal_running(PtTerminal *t);   /* fg process other than the shell */
int pt_terminal_last_exit(PtTerminal *t);      /* -1 until the prompt reports */
char *pt_terminal_current_cwd(PtTerminal *t);  /* /proc/<pid>/cwd, caller frees */
void pt_terminal_paste(PtTerminal *t);          /* async clipboard paste */
void pt_terminal_copy(PtTerminal *t);           /* copy selection to clipboard */
const char *pt_terminal_last_command(PtTerminal *t);  /* fg comm; NULL before first poll */
int pt_terminal_font_size(void);            /* shared across all terminals */
void pt_terminal_set_font_size(int pts);    /* clamped; re-measures all panes */
/* Module-level terminal colors; re-applies to every live terminal and
 * redraws. Call before the first terminal exists and on every change. */
void pt_terminal_set_theme(const PtResolvedTheme *rt);
/* Family+size together; NULL family keeps the current one. */
void pt_terminal_set_font(const char *family, int pts);
/* Module-level mouse-reporting default (the `mouse-reporting` config key).
 * Applying a config re-arms every live terminal, so a pane toggled by hand
 * follows the file again the next time it changes. */
void pt_terminal_set_mouse_reporting(gboolean on);
gboolean pt_terminal_mouse_reporting(PtTerminal *t);
/* Flips this pane only — ghostty's toggle_mouse_reporting. Returns the new
 * state. */
gboolean pt_terminal_toggle_mouse_reporting(PtTerminal *t);
/* ghostty's `reset` action, this pane only: see pt_term_core_reset. The shell
 * keeps running; only the terminal's state is thrown away. */
void pt_terminal_reset(PtTerminal *t);
/* Module-level `osc52` mode: what a program in a pane may do to the clipboard
 * with OSC 52. Re-armed on every live terminal, like the one above. */
void pt_terminal_set_osc52(PtOsc52Mode mode);
/* GObject signals: "exited" (int), "title-changed" (const char*), "activity" (void),
 *                  "command-changed" (const char*) — fg program of the pane changed,
 *                  "notification" (const char* title, const char* body) — a
 *                      program asked for a desktop notification with OSC 9 or
 *                      OSC 777. Already filtered, capped and rate-limited by
 *                      the core, and never emitted while the pane is focused;
 *                      `title` is "" when the sequence carried none. */
