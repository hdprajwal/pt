#pragma once
#include <glib.h>
#include <ghostty/vt.h>
#include <sys/types.h>
#include "pt-config.h"   /* PtOsc52Mode: what OSC 52 may do to the clipboard */
#include "pt-theme.h"    /* PtColor: the cell and palette color type */

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
  /* `from_prompt` is TRUE when the title arrived behind the prompt snippet's
   * "pt-exit:<code>;" marker — the shell set it at a prompt, not a program in
   * the pane. The marker is already stripped from `title` and the code
   * recorded (see pt_term_core_last_exit). Consumers that label a pane after
   * its title need this: the prompt sets one on every single prompt. */
  void (*title)(PtTermCore *core, const char *title, gboolean from_prompt,
                gpointer user);
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
 * exec (after TERM). NULL → none. Copied; caller keeps ownership.
 * max_scrollback: how much history this core keeps, in bytes rather than
 * lines — that is what libghostty's max_scrollback counts
 * (terminal/Screen.zig), whatever its C header calls it. Per core and fixed
 * for its life, like cols/rows: the caller reads its config at spawn, which
 * is how a scrollback-limit change reaches the panes opened after it and
 * leaves the running ones alone — in ghostty too it is a new-surface
 * setting. */
PtTermCore *pt_term_core_new(const char *cwd, const char *const *argv,
                             const char *const *env_pairs,
                             guint16 cols, guint16 rows,
                             int cell_w, int cell_h, gsize max_scrollback,
                             GError **error);
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

/* ---- mouse selection (viewport-relative pixels, inside the pane's grid
 * inset — see pt_term_core_set_padding) ---- */
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
/* `notches` wheel presses of the same button at the same point, in one write.
 * A wheel event carries several notches and a touchpad delivers them faster
 * still; the bytes are exactly what that many pt_term_core_mouse_report()
 * calls produce, only the syscall count differs. */
gboolean pt_term_core_wheel_report(PtTermCore *c, GhosttyMouseButton button,
                                   GhosttyMods mods, double px, double py,
                                   int notches);
/* Release every button the core still thinks is held, as one release report
 * each at (px, py). For the gesture endings that never reach the release path
 * — GTK cancels a click gesture for a starting drag, a popup's grab or the
 * widget's teardown, and no release event follows. The core keeps a held
 * button substituted into unnamed motion (see mouse_report), so a press bit
 * nobody clears turns every later hover into a phantom SGR drag under mode
 * 1002 — the wheel-press variant of the same leak is documented at the
 * buttons_down comment in pt_term_core_wheel_report. A no-op when nothing is
 * held. Returns TRUE when any report reached the pty. */
gboolean pt_term_core_mouse_cancel(PtTermCore *c, GhosttyMods mods,
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

/* ---- synchronized output (mode 2026, BSU/ESU) ----
 *
 * TRUE means the running program has asked the terminal to hold the frame it
 * is currently drawing off the screen until it says otherwise — pt answers
 * DECRQM for this mode as "recognised", so this is where that promise gets
 * kept. The core tracks the mode itself, on the same one-poll-per-read-batch
 * basis as focus and in-band-resize above; it does not repaint anything.
 * Answered from a shadow that self-clears on a real ESU, a safety timer if the
 * app goes quiet mid-frame, or an absolute ceiling if it never stops — see the
 * block comment above poll_mode_edges in the .c file for why both exist. */
gboolean pt_term_core_sync_output(PtTermCore *c);

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

/* ---- grid inset ----
 *
 * Pixels between the pane's edge and the first cell, defaulting to
 * PT_CONFIG_WINDOW_PADDING_{X,Y}_DEFAULT. The widget owns this value — it is
 * the one drawing the grid — and pushes it here so the core's pixel-to-cell
 * mapping (selection, links, the geometry the mouse encoder reports from)
 * stays in step with what is on screen. Every pixel a caller passes is a pane
 * coordinate, so a core told the wrong inset answers off by whole cells. */
void pt_term_core_set_padding(PtTermCore *c, int x, int y);

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

/* The five SGR underline shapes plus "none", mirroring GhosttySgrUnderline
 * (build/_deps/ghostty-src/include/ghostty/vt/sgr.h:99-105) value for value —
 * fill_cell copies style.underline into this straight, no translation table
 * to keep in sync when libghostty adds one. */
typedef enum {
  PT_UNDERLINE_NONE   = 0,
  PT_UNDERLINE_SINGLE = 1,
  PT_UNDERLINE_DOUBLE = 2,
  PT_UNDERLINE_CURLY  = 3,
  PT_UNDERLINE_DOTTED = 4,
  PT_UNDERLINE_DASHED = 5,
} PtUnderline;

/* ---- flat cell rows ----
 *
 * The renderer-facing view of one visible row, flattened into plain memory so
 * a frame iterates arrays instead of making several FFI calls per cell.
 * Everything here answers as of the last pt_term_core_sync(). */
#define PT_CELL_TEXT_MAX 64
typedef struct {
  char     text[PT_CELL_TEXT_MAX]; /* full UTF-8 cluster, NUL-terminated; "" = blank */
  guint8   width;                  /* 1 or 2; 0 = spacer tail of a wide cell */
  guint16  style;                  /* PT_CELL_STYLE_* bits; the widget draws from bold/italic/inverse today, the rest await a renderer */
  guint8   underline;              /* a PtUnderline value; PT_UNDERLINE_NONE when unset */
  gboolean selected;
  /* An OSC 8 hyperlink hangs off this cell — any link, openable or not, since
   * this is what the underline draws from and the underline is the only thing
   * telling the user the run is a link at all. Whether pt will *open* it is
   * pt_term_core_link_at_cell's question, not this one's. Independent of
   * `underline`: a linked cell with no SGR underline still gets pt's link
   * underline, and an SGR-underlined cell that happens to be a link keeps
   * both meanings on the same rule. */
  gboolean has_link;
  gboolean has_bg;
  /* The color SGR 58 set for the underline itself, distinct from `fg` —
   * ghostty's underline_color style field, resolved (palette or RGB) the way
   * fg is, but unlike fg there is no libghostty default to fall back to, so
   * FALSE here means "draw the underline in the cell's own fg", same as a
   * plain terminal. */
  gboolean has_underline_color;
  PtColor  underline_color;        /* valid only when has_underline_color */
  PtColor  fg;
  PtColor  bg;                     /* valid only when has_bg */
} PtCell;

/* PtCell.style bits — the GhosttyStyle properties a renderer draws from.
 * The five SGR underline styles live in PtCell.underline instead of a bit
 * here, since "on" is not enough to draw one. `inverse` is reported, not
 * applied: fg/bg hold the cell's own colors and the consumer swaps them,
 * exactly as the widget's snapshot loop does today. */
#define PT_CELL_STYLE_BOLD      ((guint16)(1u << 0))
#define PT_CELL_STYLE_ITALIC    ((guint16)(1u << 1))
#define PT_CELL_STYLE_STRIKE    ((guint16)(1u << 2))
#define PT_CELL_STYLE_FAINT     ((guint16)(1u << 3))
#define PT_CELL_STYLE_INVERSE   ((guint16)(1u << 4))
#define PT_CELL_STYLE_BLINK     ((guint16)(1u << 5))
#define PT_CELL_STYLE_INVISIBLE ((guint16)(1u << 6))
#define PT_CELL_STYLE_OVERLINE  ((guint16)(1u << 7))

/* Fills out[0..n) for visible row `row` (0-based). Returns number of cells
 * filled (min(cols, max)). One libghostty walk per call; caller owns the array.
 *
 * `fg` is already resolved: a cell with no color of its own reports the
 * default foreground the render state carries, so a renderer never has to ask
 * twice. `bg` is not — the default background is painted once for the whole
 * pane, so `has_bg` says whether this cell diverges from it. `underline_color`
 * is resolved the same way fg is when set (palette index or RGB, whichever
 * the program used), but unlike fg has no default to fall back to, so a cell
 * that never got an SGR 58 reports has_underline_color FALSE rather than a
 * guessed color. A row past the viewport, or a walk the library refuses,
 * fills nothing and returns 0. */
int pt_term_core_row_cells(PtTermCore *c, int row, PtCell *out, int max);

/* ---- sequential row walk ----
 *
 * pt_term_core_row_cells seeks from the top on every call, so reading a whole
 * frame with it costs O(rows²) iterator steps. A reader walks every visible
 * row in one pass instead: begin at the top, next() points *out at the next
 * row's cells (identical to what row_cells reports for that row, every cell,
 * however wide the row — the reader owns the buffer and grows it, so nothing
 * is ever truncated) and returns the count, or -1 past the last row. A row
 * the library refuses to read returns 0 and the walk continues. The cells
 * behind *out belong to the reader and are valid until the next next() or
 * end(). begin() returns NULL when the walk cannot start at all. Same sync
 * contract as row_cells; end() is NULL-safe. */
typedef struct PtRowReader PtRowReader;
PtRowReader *pt_term_core_rows_begin(PtTermCore *c);
int pt_term_core_rows_next(PtRowReader *r, const PtCell **out);
void pt_term_core_rows_end(PtRowReader *r);

/* Whether any cell of visible row `row` carries an OSC 8 link — the row flag
 * libghostty maintains (false positives allowed, false negatives never), so a
 * renderer can skip the per-cell question on rows that answer no. As of the
 * last sync. */
gboolean pt_term_core_row_has_link(PtTermCore *c, int row);
/* The URI at visible cell (row, col), under exactly the rules of
 * pt_term_core_hyperlink_at — safe schemes only, caller g_free's — just
 * addressed by cell instead of by pixel. NULL out of range, on no link, and
 * on a link pt will not open (a cell can have has_link set and still answer
 * NULL here; the underline is honest, the hand cursor is cautious). */
char *pt_term_core_link_at_cell(PtTermCore *c, int row, int col);

/* ---- logical lines ----
 *
 * What a bare-URL match runs against. OSC 8 needs none of this — the program
 * said where its link starts and ends — but an address a program merely
 * printed is just cells, so the matcher needs the row back as a string, and
 * the string's bytes back as cells once it has a match.
 *
 * A row a program wrapped is not a line: a URL that ran off the right edge
 * continues on the next row, and matching row by row would cut it in half. So
 * the rows libghostty marks as wrapped are joined here, and every row of the
 * group answers with the same line — ghostty does the same by matching against
 * a selected logical line (Surface.zig linkAtPin). */
typedef struct {
  gint16 row;                 /* visible row, 0-based */
  gint16 col;                 /* column within that row */
} PtCellPos;

typedef struct {
  char      *text;   /* the line as UTF-8, NUL-terminated; blanks are spaces */
  PtCellPos *at;     /* `len` entries: at[i] is the cell byte i was drawn by */
  gsize      len;    /* bytes in text, not counting the NUL */
} PtLine;

/* The logical line containing visible row `row`, as of the last sync. FALSE
 * (leaving *out untouched) for a row outside the viewport or a walk the
 * library refuses. Caller clears it with pt_term_core_line_clear.
 *
 * Blank cells become spaces rather than being trimmed, so on a line of plain
 * ASCII a byte offset and a column are the same number. Anything wider breaks
 * that — a multi-byte cluster is several bytes of one cell, and the spacer
 * half of a wide character contributes none — which is what `at` is for: it
 * is the general answer, and the arithmetic is not. */
gboolean pt_term_core_line_at(PtTermCore *c, int row, PtLine *out);
void pt_term_core_line_clear(PtLine *l);

typedef struct {
  PtColor bg, fg, cursor;
  PtColor palette[16];
} PtTermColors;
/* Push a theme's terminal colors into libghostty: default bg/fg/cursor plus
 * the ANSI slots the theme pins. A palette slot with alpha 0 is "not pinned"
 * and keeps libghostty's stock default for that slot — matching the theme's
 * palette_set flags — and OSC 4 overrides a program set survive either way. */
void pt_term_core_set_colors(PtTermCore *c, const PtTermColors *colors);

/* The effective default background and foreground — the theme's, unless a
 * program moved them with OSC 11/10 — as of the last sync. Written only on a
 * successful read, so callers seed the out params with their fallbacks. */
void pt_term_core_default_colors(PtTermCore *c, PtColor *bg, PtColor *fg);

typedef struct {
  int      x, y;        /* cell coords; y in viewport rows */
  int      style;       /* GhosttyRenderStateCursorVisualStyle values: DECSCUSR
                         * pairs 1/2 block, 3/4 underline, 5/6 bar (odd blinking,
                         * even steady), 0 back to a steady block — ghostty's
                         * numbering (src/terminal/stream.zig:1593) */
  gboolean visible;
  gboolean blinking;
  /* The terminal believes the cursor sits at a password prompt: draw something
   * unmissable and never blink it. Nothing in pt can set this yet (ghostty
   * polls the pty's termios and libghostty-vt's C API has no way in), so today
   * it is always FALSE; the renderer handles it anyway. */
  gboolean password;
  int      width;       /* 1 or 2 (wide glyph under cursor) */
  PtColor  color;       /* the cursor color a program set, or the default fg */
} PtCursorInfo;
/* One call for everything the cursor drawing needs. Answers as of the last
 * pt_term_core_sync(), like the flat rows — sync first, then ask. FALSE when
 * the cursor is outside the viewport (scrolled away), and then x/y/width are
 * meaningless; the other fields are filled either way. On the spacer tail of
 * a wide character x is already backed up onto the head and width is 2, so
 * the caller draws at x without asking twice (ghostty tests tail then head,
 * renderer/generic.zig:3232). */
gboolean pt_term_core_cursor_info(PtTermCore *c, PtCursorInfo *out); /* one row walk max, no sync */

/* TRUE exactly once after terminal content/viewport changed since the last call. */
gboolean pt_term_core_take_render_dirty(PtTermCore *c);

/* TRUE when the caller should call pt_term_core_sync(): the render state has
 * moved since the last frame and no program is holding this one back with
 * synchronized output. The dirty flag is consumed only when the answer is
 * TRUE — a flag taken during a hold would be thrown away, and the first frame
 * after the ESU would find nothing left to pick up. */
gboolean pt_term_core_take_frame(PtTermCore *c);

/* The counter behind take_render_dirty: bumped on every content, viewport,
 * color or selection change, never by readers (take included). Two equal
 * reads mean nothing to re-derive in between; monotonic, wraps at G_MAXUINT. */
guint pt_term_core_content_serial(PtTermCore *c);

void pt_term_core_sync(PtTermCore *c);              /* render_state_update */
char *pt_term_core_grid_text(PtTermCore *c);        /* visible grid, caller frees */
/* The UTF-8 text of the last visible row holding a non-blank cell, trailing
 * blanks stripped, copied into buf (cap bytes, NUL included — text past the
 * cap is dropped on a codepoint boundary). FALSE, with buf set to "", when
 * every row is blank. Allocates nothing, so it may be asked per frame; like
 * pt_term_core_grid_text it answers as of the last pt_term_core_sync(). */
gboolean pt_term_core_last_nonempty_row(PtTermCore *c, char *buf, gsize cap);
gboolean pt_term_core_exited(PtTermCore *c, int *status);
pid_t pt_term_core_shell_pid(PtTermCore *c);
/* Basename of the program this core spawned (argv[0], or the resolved default
 * shell) — derived from the spawn itself, not read back from /proc, so it
 * cannot race the child's exec. Set once at spawn, never NULL, owned by the
 * core; valid until pt_term_core_free. */
const char *pt_term_core_shell_name(PtTermCore *c);
/* The random token this core's child was given as $PT_PANE_TOKEN — the key
 * agent integrations write their session reports under. Fresh per spawn,
 * meaningless across pt restarts. Owned by the core; valid until free. */
const char *pt_term_core_pane_token(PtTermCore *c);
/* TRUE when a foreground process other than the shell owns the tty. Answered
 * from a cached field the 700ms foreground poll maintains (seeded TRUE at
 * spawn, cleared on child exit), so it is free to ask every frame; the answer
 * can be up to one poll interval old. */
gboolean pt_term_core_running(PtTermCore *c);
/* Last exit code reported via the "pt-exit:<n>;" title marker; -1 before
 * the prompt snippet ever reports. */
int pt_term_core_last_exit(PtTermCore *c);
void pt_term_core_free(PtTermCore *c);
