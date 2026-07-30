#pragma once
#include <gtk/gtk.h>
#include "pt-config.h"
#include "pt-term-core.h"
#include "pt-theme.h"

#define PT_TYPE_TERMINAL (pt_terminal_get_type())
G_DECLARE_FINAL_TYPE(PtTerminal, pt_terminal, PT, TERMINAL, GtkWidget)

GtkWidget *pt_terminal_new(const char *cwd);
/* env_pairs: NULL-terminated "KEY=VALUE" strings for the child, copied (NULL
 * clears). Read once, when the pane spawns its shell — which happens lazily, at
 * the pane's first allocation — so setting it afterwards reaches only a
 * respawn: a live shell keeps the env it started with. The pane grid calls this
 * for every pane it builds, with the env its window handed it. */
void pt_terminal_set_spawn_env(PtTerminal *t, const char *const *env_pairs);
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
/* The shell's own name ("zsh"), not the foreground command — derived from the
 * spawn itself (never a /proc read, which could race the child's exec). NULL
 * only while the pane has no live core: before first spawn, or after a failed
 * respawn. Borrowed from the core; do not free or hold across a restart. */
const char *pt_terminal_shell_name(PtTerminal *t);
int pt_terminal_font_size(void);            /* shared across all terminals */
void pt_terminal_set_font_size(int pts);    /* clamped; re-measures all panes */
/* Module-level terminal colors; re-applies to every live terminal and
 * redraws. Call before the first terminal exists and on every change. */
void pt_terminal_set_theme(const PtResolvedTheme *rt);
/* Family+size together; NULL family keeps the current one. */
void pt_terminal_set_font(const char *family, int pts);
/* The `mouse-reporting` config key, pushed into every live terminal: a pane
 * toggled by hand follows the file again the next time the config is applied.
 * Nothing is remembered between calls, so a pane built later gets its value
 * from its grid instead (pt_pane_grid_set_pane_defaults), through the one-pane
 * form below. */
void pt_terminal_set_mouse_reporting(gboolean on);
/* This pane only, for a pane built after the config was applied: arming it
 * through the broadcast above would drag every other pane back into line with
 * the file, undoing a hand toggle no config change asked to undo. */
void pt_terminal_set_pane_mouse_reporting(PtTerminal *t, gboolean on);
gboolean pt_terminal_mouse_reporting(PtTerminal *t);
/* Flips this pane only — ghostty's toggle_mouse_reporting. Returns the new
 * state. */
gboolean pt_terminal_toggle_mouse_reporting(PtTerminal *t);
/* ghostty's `reset` action, this pane only: see pt_term_core_reset. The shell
 * keeps running; only the terminal's state is thrown away. */
void pt_terminal_reset(PtTerminal *t);
/* The `osc52` config key: what a program in a pane may do to the clipboard with
 * OSC 52. Pushed into every live terminal, like the one above, and paired with
 * the same one-pane form. */
void pt_terminal_set_osc52(PtOsc52Mode mode);
void pt_terminal_set_pane_osc52(PtTerminal *t, PtOsc52Mode mode);
/* GObject signals: "exited" (int), "title-changed" (const char*),
 *                  "command-changed" (const char*) — fg program of the pane changed,
 *                  "notification" (const char* title, const char* body) — a
 *                      program asked for a desktop notification with OSC 9 or
 *                      OSC 777. Already filtered, capped and rate-limited by
 *                      the core, and never emitted while the pane is focused;
 *                      `title` is "" when the sequence carried none. */
