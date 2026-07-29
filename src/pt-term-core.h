#pragma once
#include <glib.h>
#include <ghostty/vt.h>
#include <sys/types.h>
#include "pt-config.h"   /* PtOsc52Mode: what OSC 52 may do to the clipboard */

typedef struct PtTermCore PtTermCore;

typedef struct {
  void (*draw)(PtTermCore *core, gpointer user);   /* state changed; redraw */
  /* Bytes arrived from the child and the parser has consumed them. Fires just
   * before the draw that follows, and only for real output: a scroll, a reset
   * and anything else that only moves pt's view of the terminal fire `draw`
   * alone. Anything that has to tell output apart from a redraw — the cursor
   * blink phase, which output pushes back to visible — belongs here. */
  void (*output)(PtTermCore *core, gpointer user);
  void (*exited)(PtTermCore *core, int status, gpointer user);
  void (*title)(PtTermCore *core, const char *title, gpointer user);
  void (*command)(PtTermCore *core, const char *comm, gpointer user);
  /* An OSC sequence the shell or an app emitted, scanned off the pty stream
   * because libghostty parses OSC but hands almost none of it back. `code` is
   * the number before the first ';', `payload` everything after it (NUL-
   * terminated as well as counted, valid for the call only). Every code pt
   * sees arrives here — switch on the ones you want and ignore the rest.
   * With this and clipboard_write both NULL the scanner does not run at all. */
  void (*osc)(PtTermCore *core, int code, const char *payload, gsize len,
              gpointer user);
  /* A program asked to put `text` on the clipboard with OSC 52 (`primary`
   * TRUE when it asked for the primary selection rather than the clipboard
   * proper). Already decoded, size-capped and checked: `text` is NUL-
   * terminated, `len` bytes long, holds no NUL of its own, is valid UTF-8 (a
   * clipboard is offered to the desktop as text), and is valid for the call
   * only. Nothing has touched the real clipboard yet — that is the
   * consumer's call, and so is asking the user first. Never fires for the
   * read form of OSC 52; see pt_term_core_set_osc52(). */
  void (*clipboard_write)(PtTermCore *core, const char *text, gsize len,
                          gboolean primary, gpointer user);
  /* A program asked for a desktop notification with OSC 9 or OSC 777 — a
   * build that finished while the pane was somewhere the user is not looking.
   *
   * Everything the payload had to survive has already happened: the ConEmu
   * extensions that share OSC 9 are gone, the text is valid UTF-8 and capped
   * (see PT_NOTIFY_TITLE_MAX / PT_NOTIFY_BODY_MAX), and the rate limit has
   * been paid. Both strings are NUL-terminated and valid for the call only.
   *
   * `title` is "" whenever the sequence carried none, which OSC 9 always
   * does; the consumer names the notification itself in that case.
   *
   * Never fires while the pane is focused — see pt_term_core_focus_report. A
   * notification for the pane the user is already reading is noise, so the
   * core drops it before it can cost the rate limit anything. */
  void (*notification)(PtTermCore *core, const char *title, const char *body,
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
/* A no-op when all four arguments match what the core already has. Otherwise
 * the kernel is told first (TIOCSWINSZ, and the SIGWINCH that follows it) and
 * the terminal second, which is also when an app on mode 2048 gets its in-band
 * size report — libghostty-vt writes that one itself. Callers may fire this on
 * every layout pass; the guard is here rather than at the call sites. */
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

/* ---- where the viewport sits (what a scrollbar draws) ----
 *
 * All three are row counts on the active screen: `total` is the whole
 * scrollable area, `offset` the distance from its top to the first visible
 * row, `len` the visible area. So a view sitting at the bottom has
 * offset + len == total, and a screen with nothing above it has total == len,
 * which is the case where there is no bar to draw. Any of the three may be
 * NULL. FALSE when the library refuses the query, and then nothing is written.
 *
 * The library warns that this is expensive to compute when the viewport is at
 * an arbitrary position — it walks the page list to find the offset — so the
 * answer is cached and only re-read when the viewport moved or the terminal
 * was written to since the last read. Callers may therefore ask once per
 * frame and pay for it only on the frames where it changed. Ghostty rate-
 * limits the same read the same way and for the same reason, by taking it
 * once per frame data update rather than on demand
 * (renderer/generic.zig:1211-1216). */
gboolean pt_term_core_scrollbar(PtTermCore *c, guint64 *total, guint64 *offset,
                                guint64 *len);

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

/* ---- focus reporting (mode 1004) ----
 *
 * Tell the app the pane gained or lost focus (CSI I / CSI O). The core
 * remembers the state whether or not mode 1004 is on, so a program that
 * enables it later is told the truth right away: the core watches for that
 * enable itself and resends, as ghostty does.
 *
 * The state is remembered even when nothing is written, so `force` exists for
 * the one caller that must report an unchanged state — the resend above.
 * Returns TRUE when bytes reached the pty. */
gboolean pt_term_core_focus_report(PtTermCore *c, gboolean focused,
                                   gboolean force);

gboolean pt_term_core_alt_screen(PtTermCore *c);   /* alternate screen active */
gboolean pt_term_core_alt_scroll(PtTermCore *c);   /* mode 1007, default on */
/* Wheel on the alt screen with alt-scroll on: `count` cursor-key arrows,
 * application or normal form per DECCKM. */
void pt_term_core_send_arrows(PtTermCore *c, gboolean up, int count);

/* ---- clipboard writes from programs (OSC 52) ----
 *
 * PT_OSC52_OFF drops them; every other mode decodes and hands them to the
 * clipboard_write callback, which decides what to do with them. Defaults to
 * PT_CONFIG_OSC52_DEFAULT so a core nobody configures behaves like the shipped
 * config. Nothing here answers the *read* form (`ESC ] 52 ; c ; ? BEL`) at any
 * setting: replying means writing whatever the user has copied back down a pty
 * that some remote program is reading, which is exfiltration with extra steps.
 * A query is dropped in silence — no callback, and not one byte to the pty. */
void pt_term_core_set_osc52(PtTermCore *c, PtOsc52Mode mode);

/* ---- color scheme (CSI ? 996 n, mode 2031) ----
 *
 * What pt tells programs about the light/dark question, so a TUI can pick a
 * palette that matches the pane it is drawing into. A query is always answered;
 * a change is announced only while mode 2031 is on, and only when the value
 * really changed — a caller may fire this on every theme apply. New cores start
 * dark, which is the theme pt ships.
 *
 * ghostty reports the *desktop* preference (AdwStyleManager:dark) and only
 * seeds that from its background's luminance at startup. pt reports its own
 * active theme's luminance directly: with no window-theme setting and no Adw
 * plumbing, routing through the desktop would have a light desktop tell apps
 * "light" while pt paints its dark theme — the opposite of useful for an app
 * trying to match. Same answer as ghostty's default, one step shorter. */
void pt_term_core_set_color_scheme(PtTermCore *c, gboolean dark);

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
/* ---- full reset (RIS) ----
 *
 * ghostty's `reset` action: modes, screens, scrollback, screen contents,
 * tabstops, scrolling region, title and selection all back to defaults, with
 * the viewport snapped to the bottom. Dimensions and colors are kept, and the
 * child is not touched — this resets pt's view of the terminal, not the shell,
 * so a wedged program can be unwedged without losing the session. pt's own
 * mirrors of that state (selection, held buttons, the OSC scanner) go with it.
 * Fires the draw callback. */
void pt_term_core_reset(PtTermCore *c);

/* ---- cursor shape and blink (DECSCUSR, mode 12) ----
 *
 * What the app asked the cursor to look like, so a renderer can draw an insert
 * bar in nvim rather than the same block everywhere. DECSCUSR pairs a shape
 * with a blink: 1/2 block, 3/4 underline, 5/6 bar, odd blinking and even
 * steady, and 0 (or an empty parameter) back to the default, which for a
 * terminal with no user config is a steady block — ghostty's own numbering
 * (src/terminal/stream.zig:1593, tests at :2833) and its VT-only mapping
 * (src/terminal/stream_terminal.zig:154). Mode 12 carries the blink on its own.
 *
 * All four read the render state, so they answer as of the last
 * pt_term_core_sync() — the same contract as pt_term_core_render_state() and
 * the row iterators. Sync first, then ask. */
GhosttyRenderStateCursorVisualStyle pt_term_core_cursor_style(PtTermCore *c);
gboolean pt_term_core_cursor_blinking(PtTermCore *c);
/* TRUE while the terminal believes the cursor sits at a password prompt, which
 * is a cue to draw something unmissable and never blink it. Nothing in pt sets
 * this yet: ghostty decides it by polling the pty's termios for canonical mode
 * with echo off (src/termio/Exec.zig:366), and libghostty-vt's C API has no way
 * in, so today it is always FALSE. The renderer handles it anyway, so the day
 * pt grows that poll the drawing is already right. */
gboolean pt_term_core_cursor_password_input(PtTermCore *c);
/* TRUE when the cursor sits on the second half of a wide character. The cell
 * it is standing on holds nothing of its own — the glyph belongs to the cell
 * to its left — so a renderer should back up one column and draw two cells
 * wide, as ghostty does (src/renderer/generic.zig:3232). */
gboolean pt_term_core_cursor_wide_tail(PtTermCore *c);
/* TRUE when the cursor is standing on the *head* of a wide character — the
 * other half of the same problem, and the one the render state does not
 * answer, so this walks to the cursor's cell and asks it. A renderer covers
 * two cells for this one without backing up. Ghostty tests the same two things
 * in the same order (renderer/generic.zig:3232): tail first, head second.
 *
 * Walks with the core's shared row iterator, exactly as pt_term_core_grid_text
 * does, so do not call it from inside a walk of your own. */
gboolean pt_term_core_cursor_wide(PtTermCore *c);

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
