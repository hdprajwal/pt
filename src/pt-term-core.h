#pragma once
#include <glib.h>
#include <ghostty/vt.h>
#include <sys/types.h>

typedef struct PtTermCore PtTermCore;

typedef struct {
  void (*draw)(PtTermCore *core, gpointer user);   /* state changed; redraw */
  void (*exited)(PtTermCore *core, int status, gpointer user);
  void (*title)(PtTermCore *core, const char *title, gpointer user);
  void (*command)(PtTermCore *core, const char *comm, gpointer user);
  /* An OSC sequence the shell or an app emitted, scanned off the pty stream
   * because libghostty parses OSC but hands almost none of it back. `code` is
   * the number before the first ';', `payload` everything after it (NUL-
   * terminated as well as counted, valid for the call only). Every code pt
   * sees arrives here — switch on the ones you want and ignore the rest.
   * Set to NULL and the scanner does not run at all. */
  void (*osc)(PtTermCore *core, int code, const char *payload, gsize len,
              gpointer user);
} PtTermCoreCallbacks;

/* argv NULL → spawn the user's shell ($SHELL → passwd → /bin/sh).
 * env_pairs: NULL-terminated "KEY=VALUE" strings set in the child before
 * exec (after TERM). NULL → none. Copied; caller keeps ownership. */
PtTermCore *pt_term_core_new(const char *cwd, const char *const *argv,
                             const char *const *env_pairs,
                             guint16 cols, guint16 rows,
                             int cell_w, int cell_h, GError **error);
void pt_term_core_set_callbacks(PtTermCore *c, const PtTermCoreCallbacks *cbs,
                                gpointer user);
void pt_term_core_resize(PtTermCore *c, guint16 cols, guint16 rows,
                         int cell_w, int cell_h);
void pt_term_core_write(PtTermCore *c, const char *buf, gssize len);
/* Returns TRUE if the encoder produced bytes (event was consumed). */
gboolean pt_term_core_send_key(PtTermCore *c, GhosttyKey key,
                               GhosttyKeyAction action, GhosttyMods mods,
                               guint32 unshifted_cp,
                               const char *utf8, gsize utf8_len);
void pt_term_core_scroll_delta(PtTermCore *c, int rows);
/* Snap the viewport back to the active area (what typing should do). */
void pt_term_core_scroll_bottom(PtTermCore *c);

/* ---- mouse selection (viewport-relative pixels; PT_PAD_X/PT_PAD_Y-inset) ---- */
void pt_term_core_selection_press(PtTermCore *c, double px, double py,
                                  guint64 time_ns);
void pt_term_core_selection_drag(PtTermCore *c, double px, double py);
void pt_term_core_selection_release(PtTermCore *c, double px, double py);
void pt_term_core_selection_clear(PtTermCore *c);
char *pt_term_core_selection_text(PtTermCore *c);  /* NULL when no selection; caller g_free */

/* ---- OSC 8 hyperlinks (same pixel space as the selection calls) ----
 *
 * The URI a program attached to the cell under the pointer, or NULL when there
 * is no cell there, no link on it, or the link is not one pt opens. Caller
 * g_free's it. Only the schemes below come back, so a URI from here is always
 * safe to hand to the desktop. */
char *pt_term_core_hyperlink_at(PtTermCore *c, double px, double py);
/* http, https, file and mailto, case-insensitively, and nothing else: the URI
 * comes from whatever wrote to the pty, so anything that hands a shell command
 * or a local helper its arguments (ssh:, smb:, and every scheme some installed
 * app claims) stays unopenable. Control bytes are rejected too — a URI is
 * never allowed to carry a newline into whatever runs it. */
gboolean pt_term_core_hyperlink_is_safe(const char *uri);

/* ---- mouse reporting (modes 9/1000/1002/1003; same pixel space as above) ----
 *
 * When an app tracks the mouse it owns the pointer: wheel, buttons and motion
 * are encoded in whatever format the app asked for (X10/SGR/URxvt/SGR-pixels)
 * and written to the pty instead of driving selection or the viewport.
 * Pass GHOSTTY_MOUSE_BUTTON_UNKNOWN for bare motion (no button held).
 * Returns TRUE when the encoder produced bytes (event was consumed) — not
 * every event reports, e.g. motion inside the same cell, or motion at all in
 * press/release-only modes. */
gboolean pt_term_core_mouse_report(PtTermCore *c, GhosttyMouseAction action,
                                   GhosttyMouseButton button, GhosttyMods mods,
                                   double px, double py);
gboolean pt_term_core_mouse_tracking(PtTermCore *c);
gboolean pt_term_core_alt_screen(PtTermCore *c);   /* alternate screen active */
gboolean pt_term_core_alt_scroll(PtTermCore *c);   /* mode 1007, default on */
/* Wheel on the alt screen with alt-scroll on: `count` cursor-key arrows,
 * application or normal form per DECCKM. */
void pt_term_core_send_arrows(PtTermCore *c, gboolean up, int count);

gboolean pt_term_core_bracketed_paste(PtTermCore *c);
/* ---- paste ----
 *
 * The only place bracketed paste is written. `text` is sanitized before it
 * reaches the pty: control bytes (NUL, ESC, DEL, the tty's own signal
 * characters) become spaces, so clipboard text carrying its own ESC [ 201 ~
 * cannot end the paste early and have the rest run as typed input. With mode
 * 2004 off, newlines become carriage returns instead. len < 0 → NUL-terminated.
 * The caller keeps ownership of `text` and it is not modified. */
void pt_term_core_paste(PtTermCore *c, const char *text, gssize len);
/* FALSE when the text holds a line break — LF, or a bare CR, which the encoder
 * passes through and the tty maps back to LF — or an end-of-paste sequence,
 * i.e. when pasting it into a shell can run a command. Worth a confirmation. */
gboolean pt_term_core_paste_is_safe(const char *text, gssize len);
void pt_term_core_sync(PtTermCore *c);              /* render_state_update */
GhosttyTerminal pt_term_core_terminal(PtTermCore *c);
GhosttyRenderState pt_term_core_render_state(PtTermCore *c);
GhosttyRenderStateRowIterator pt_term_core_row_iter(PtTermCore *c);
GhosttyRenderStateRowCells pt_term_core_row_cells(PtTermCore *c);
char *pt_term_core_grid_text(PtTermCore *c);        /* visible grid, caller frees */
gboolean pt_term_core_exited(PtTermCore *c, int *status);
pid_t pt_term_core_shell_pid(PtTermCore *c);
/* TRUE when a foreground process other than the shell owns the tty. */
gboolean pt_term_core_running(PtTermCore *c);
/* Last exit code reported via the "pt-exit:<n>;" title marker; -1 before
 * the prompt snippet ever reports. */
int pt_term_core_last_exit(PtTermCore *c);
void pt_term_core_free(PtTermCore *c);
