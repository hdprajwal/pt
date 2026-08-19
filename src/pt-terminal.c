#include "pt-terminal.h"
#include "pt-block.h"
#include "pt-config.h"       /* mouse-reporting and osc52 defaults */
#include "pt-keymap.h"
#include "pt-link.h"         /* bare URLs in plain output, matched on hover */
#include "pt-session.h"      /* PT_FONT_SIZE_DEFAULT, shared with persistence */
#include <adwaita.h>         /* paste confirmation dialog */
#include <math.h>

#define PT_FONT_FAMILY_DEFAULT "JetBrains Mono"
#define PT_FONT_SIZE_MIN 6
#define PT_FONT_SIZE_MAX 32

/* Overlay scrollbar for the scrollback: a thumb on the right edge with no
 * gutter behind it, at full strength while the viewport is moving and faded
 * out once it stops. The width, the inset and the shortest the thumb may get
 * are the sidebar slider's, so the two read as the same bar (style.css, the
 * `.pt-sidebar scrollbar slider` rule) — a terminal draws itself rather than
 * being styled, so the numbers are repeated here rather than shared. */
#define PT_BAR_W       5
#define PT_BAR_MARGIN  2
#define PT_BAR_MIN_H   24
#define PT_BAR_HOLD_MS 700
#define PT_BAR_FADE_MS 400.0

/* One font size shared by every terminal: zoom is global, not per-pane. Live
 * widgets register here so a size change can re-measure them all. */
static int font_size_pts = PT_FONT_SIZE_DEFAULT;
static GSList *live_terminals;

/* Inset between the pane edge and the character grid, the `window-padding-x`
 * and `window-padding-y` config keys. Shared like the font size, and mirrored
 * into every core (pt_term_core_set_padding) so hit-testing lands on the cell
 * the user is looking at. */
static int pad_x = PT_CONFIG_WINDOW_PADDING_X_DEFAULT;
static int pad_y = PT_CONFIG_WINDOW_PADDING_Y_DEFAULT;

/* Bytes of history the next core is built with, the `scrollback-limit` key.
 * Shared like the padding above, but read only at spawn: the library takes it
 * at terminal creation and never after, so unlike the inset a change cannot
 * reach a live core — it reaches the panes opened after it, as in ghostty,
 * where scrollback-limit is a new-surface setting too. */
static gsize scrollback_limit = PT_CONFIG_SCROLLBACK_LIMIT_DEFAULT;

/* Terminal colors from the active theme. Defaults mirror pt-dark so a
 * terminal created before the first set_theme call renders correctly. */
static PtColor th_bg  = {0x0b, 0x0d, 0x10, 1.0};
static PtColor th_fg  = {0xd6, 0xda, 0xe0, 1.0};
static PtColor th_cursor = {0xd6, 0xda, 0xe0, 1.0};
static PtColor th_sel = {0x26, 0x4f, 0x38, 1.0};
static PtColor th_ring = {0x2f, 0x4f, 0x3a, 1.0};
/* The overlay scrollbar's thumb: the chrome's slider token, so the bar over a
 * pane is the same colour as the one beside the project list. */
static PtColor th_slider = {0xff, 0xff, 0xff, 0.12};
static PtColor th_pal[16] = {
  [1]  = {0xf2, 0x77, 0x7a, 1.0}, [2]  = {0x6e, 0xe7, 0xa0, 1.0},
  [3]  = {0xf2, 0xb2, 0x5c, 1.0}, [9]  = {0xf2, 0x77, 0x7a, 1.0},
  [10] = {0x6e, 0xe7, 0xa0, 1.0}, [11] = {0xf2, 0xb2, 0x5c, 1.0},
};
static gboolean th_pal_set[16] = {
  [1] = TRUE, [2] = TRUE, [3] = TRUE,
  [9] = TRUE, [10] = TRUE, [11] = TRUE,
};
/* Whether that background is dark, which is what programs asking about the
 * color scheme are told. Same default rule: pt-dark until told otherwise. */
static gboolean th_dark = TRUE;
static char *font_family;   /* NULL -> PT_FONT_FAMILY_DEFAULT */

enum { SIG_EXITED, SIG_TITLE_CHANGED, SIG_COMMAND_CHANGED,
       SIG_NOTIFICATION, N_SIGNALS };
static guint signals[N_SIGNALS];

/* Whether the per-frame/per-event g_debug lines run at all. g_debug formats
 * its arguments before the log level is consulted, and the frame line asks
 * the clock and does arithmetic per frame — checked once here so a build
 * nobody is debugging pays one branch instead. */
static gboolean pt_debug_enabled(void) {
  static gsize once;
  static gboolean on;
  if (g_once_init_enter(&once)) {
    const char *dbg = g_getenv("G_MESSAGES_DEBUG");
    on = dbg != NULL && dbg[0] != '\0';
    g_once_init_leave(&once, 1);
  }
  return on;
}

/* Handed out by pt_terminal_id(). A desktop notification outlives the read
 * that produced it — it sits on the user's screen until they click it — so
 * what it carries back has to be something that can be looked up rather than
 * followed. An id survives the pane being closed in the meantime; a pointer
 * would not. Ghostty does the same, with the core surface's id
 * (apprt/gtk/class/surface.zig sendDesktopNotification). Never reused: a
 * 64-bit counter incremented once per pane cannot wrap in any session. */
static guint64 next_terminal_id = 1;

struct _PtTerminal {
  GtkWidget parent_instance;
  guint64 id;
  PtTermCore *core;
  char *start_cwd;
  char **env;                /* extra child env, or NULL */
  /* One line to write once the shell is up, or NULL. Set before the pane
   * spawns (restore fills it from the leaf's agent session) and cleared by the
   * spawn that consumes it, so only the first shell ever sees it. */
  char *startup_input;
  char *last_command;
  /* Last title a program in this pane set over OSC 0, verbatim. NULL once the
   * shell is back at a prompt — see core_title. Distinct from last_command:
   * the comm poll lags by up to 700ms, this lands the moment the bytes do. */
  char *last_title;
  PangoLayout *layout;
  PangoFontDescription *font_desc;
  int cell_w, cell_h;
  int baseline;              /* ascent, in px: text nodes sit on the baseline */
  /* Underline/strikethrough geometry, cell-local px offsets from the row's
   * top edge, cached at font-measure time like everything else in this block
   * — see measure_font and the ---- decorations ---- comment below for where
   * these numbers come from and why. */
  int underline_thickness, underline_y;
  int strike_thickness, strike_y;
  gboolean exited;
  int exit_status;
  gboolean focused;
  /* A paste is in flight: reserved when the clipboard read starts and released
   * when the text is pasted, dropped, or the confirmation closes. The read is
   * async, so a flag set only once the dialog is up would let a repeated ⌃⇧V
   * start a second read and stack a second dialog. */
  gboolean paste_pending;

  /* mouse: last known pointer position (wheel events report at the cursor,
   * and GTK scroll events carry no coordinates), plus the sub-row remainder
   * of smooth/touchpad scrolling. */
  double mouse_x, mouse_y;
  gboolean pointer_in;       /* the pointer is inside this pane */
  double scroll_pending;    /* sub-row remainder, local viewport scrolling */
  double report_pending;    /* sub-notch remainder, wheel reports to the app */
  gboolean reporting_drag;   /* the app owns this drag, not the selection */
  gboolean button_down;      /* a button is held: ownership is already decided */
  gboolean report_mouse;     /* this pane's copy of `mouse-reporting` */
  PtOsc52Mode osc52;         /* this pane's copy of `osc52` */
  gboolean osc52_asking;     /* a clipboard-write confirmation is up */
  gboolean link_cursor;      /* the hand cursor is up: a link is under the pointer */
  /* What update_link_cursor last answered for: the pointer's cell and the
   * core's content serial as of that answer. While neither moves the answer
   * cannot have changed, so the seat walk and the grid lookup are skipped.
   * link_row == -2 means no cache (anything that changes the answer without
   * moving either — modifiers, mouse-reporting flips, a button settling —
   * resets it there). */
  int link_col, link_row;
  guint link_serial;

  /* The bare URL under the pointer, if any: the address, and the cells it
   * covers as a range in reading order — (url_r0, url_c0) through
   * (url_r1, url_c1) inclusive, which is what a match over a wrapped line
   * looks like on screen. Resolved by the same hover pass that answers the
   * hand cursor, so the draw and the click both read it instead of matching
   * again. url_uri NULL means there is none. */
  char *url_uri;
  int url_r0, url_c0, url_r1, url_c1;

  /* overlay scrollbar: when the viewport last moved (monotonic µs, 0 = never
   * or already faded out), the timer waiting the hold out, and the tick
   * callback driving the fade after it. Only one of the last two is ever set. */
  gint64 bar_at;
  guint bar_hold;
  guint bar_tick;

  /* cursor blink. `blink_source` is the toggle timer and is non-zero exactly
   * while this pane is drawing a blinking cursor; `blink_visible` is which
   * half of the cycle we are in, and means nothing while no timer runs (it is
   * parked at TRUE, so a steady cursor is never hidden). `blink_reset_at` is
   * the last time output pushed the phase back to visible, for the debounce. */
  guint blink_source;
  gboolean blink_visible;
  gboolean blink_wanted;     /* last synced: the app asked for a blinking cursor */
  gint64 blink_reset_at;
  /* Set during the snapshot's row walk, true when the viewport just drawn
   * held at least one SGR-5 cell. Ghostty parses blink into style.flags but
   * its renderer never reads it (grep -rn "flags.blink" src/renderer/ in the
   * vendored tree is empty) — pt goes further and actually blinks the text,
   * on the same phase as the cursor, so the two never fight over the eye's
   * attention. There is no core accessor for this: the widget already walks
   * every cell for the frame it is about to draw, so answering "did this
   * frame contain one" here is free, where a second grid walk to ask the core
   * separately would not be. */
  gboolean text_blink;
};

G_DEFINE_FINAL_TYPE(PtTerminal, pt_terminal, GTK_TYPE_WIDGET)

/* Declared up here only because the places that have to re-ask what is under
 * the pointer — new output, a resize, a restarted shell — run well before the
 * mouse code they belong with. */
static void update_link_cursor(PtTerminal *t);
static void link_cache_reset(PtTerminal *t);
static void url_clear(PtTerminal *t);

/* ---- cursor blink ----
 *
 * ghostty toggles the cursor every 600ms from a timer it runs only while the
 * surface is focused (renderer/Thread.zig:20 and :401-429), so the full cycle
 * is 600 on, 600 off. It reads nothing from the desktop — no
 * gtk-cursor-blink-time — and offers no setting for the rate, and neither does
 * pt: this is one literal, matched to the reference implementation.
 *
 * The phase goes back to visible whenever output arrives, debounced to at most
 * once every 500ms (termio/Termio.zig:651-673). Hooking output rather than the
 * key event is ghostty's choice and it is the better one: it keeps the cursor
 * solid while you type into a shell that echoes you, and equally while a
 * program is redrawing itself around a cursor you are not touching, which is
 * the case a keypress hook would blink straight through. */
#define PT_CURSOR_BLINK_MS 600
#define PT_CURSOR_BLINK_RESET_MS 500

static gboolean blink_tick(gpointer user) {
  PtTerminal *t = PT_TERMINAL(user);
  t->blink_visible = !t->blink_visible;
  gtk_widget_queue_draw(GTK_WIDGET(t));
  return G_SOURCE_CONTINUE;
}

/* The two — and only two — places the timer is created and destroyed. */
static void blink_timer_stop(PtTerminal *t) {
  g_clear_handle_id(&t->blink_source, g_source_remove);
  /* Nothing may hide the cursor while no timer is running to bring it back. */
  t->blink_visible = TRUE;
}

static void blink_timer_start(PtTerminal *t) {
  blink_timer_stop(t);         /* restarting means the interval starts over */
  t->blink_source = g_timeout_add(PT_CURSOR_BLINK_MS, blink_tick, t);
}

/* The one place that decides whether a timer should exist. A blink timer runs
 * exactly while this pane is focused, still has a shell, and either the app
 * has asked for a blinking cursor or the frame just drawn held a blinking
 * cell — an unfocused pane draws a hollow block and steady text whatever it
 * was asked for, so a timer there would burn a wakeup a second to animate
 * nothing. Every input to that answer calls back here when it changes:
 * focus-in, focus-out, the sync in snapshot (where blink_wanted and
 * text_blink are both freshly known), the child exiting, and dispose. */
static void sync_blink_timer(PtTerminal *t) {
  gboolean want = t->focused && !t->exited && (t->blink_wanted || t->text_blink);
  if (want == (t->blink_source != 0)) return;
  if (want)
    blink_timer_start(t);
  else
    blink_timer_stop(t);
}

/* Output means something is happening at the cursor: show it. */
static void blink_phase_reset(PtTerminal *t) {
  gint64 now = g_get_monotonic_time();
  if (t->blink_reset_at != 0 &&
      now - t->blink_reset_at <= PT_CURSOR_BLINK_RESET_MS * 1000)
    return;                    /* a busy reader must not restart the timer per read */
  t->blink_reset_at = now;
  t->blink_visible = TRUE;
  if (t->blink_source != 0) blink_timer_start(t);
}

/* ---- core callbacks ---- */
/* Output, as opposed to a redraw: the core fires this only for bytes from the
 * child, so scrolling the viewport and snapping it back on a keypress do not
 * pass for typing. */
static void core_output(PtTermCore *core, gpointer user) {
  (void)core;
  blink_phase_reset(PT_TERMINAL(user));
}

static void core_draw(PtTermCore *core, gpointer user) {
  PtTerminal *t = PT_TERMINAL(user);
  /* A synchronized-output frame is not done yet: painting now would show the
   * half of it the app has written so far, then tear again when the rest
   * lands. Ghostty's renderer stops entirely for the same reason and the
   * same window (renderer/generic.zig:1176-1180); pt just skips the queue
   * here rather than the eventual draw call, since nothing has changed to
   * paint until the flag drops.
   *
   * This also delays repaints this callback fires for reasons that have
   * nothing to do with the pty — scrollback scrolling and selection clears
   * both route through cbs.draw — for as long as the sync window holds. That
   * matches ghostty, whose whole renderer freezes the same way, and is not a
   * bug to "fix" by carving out an exception for them. */
  if (pt_term_core_sync_output(core)) return;
  gtk_widget_queue_draw(GTK_WIDGET(t));
  update_link_cursor(t);       /* the grid just moved under the pointer */
}

static void core_exited(PtTermCore *core, int status, gpointer user) {
  (void)core;
  PtTerminal *t = PT_TERMINAL(user);
  t->exited = TRUE;
  t->exit_status = status;
  sync_blink_timer(t);         /* nothing left to point at */
  gtk_widget_queue_draw(GTK_WIDGET(t));
  g_signal_emit(t, signals[SIG_EXITED], 0, status);
}

static void core_title(PtTermCore *core, const char *title,
                       gboolean from_prompt, gpointer user) {
  (void)core;
  PtTerminal *t = PT_TERMINAL(user);
  /* A marked title is the shell at a prompt: the command that owned the pane
   * is over, so the title it set is finished with. Dropping it here rather
   * than when the foreground command next changes keeps it off the 700ms comm
   * poll — that poll runs late enough that it would sometimes wipe a title the
   * new program had already set, and the tab would keep reading "claude" for
   * seconds. */
  if (from_prompt) {
    g_clear_pointer(&t->last_title, g_free);
  } else {
    g_free(t->last_title);
    t->last_title = g_strdup(title);
  }
  /* Emitted either way. The window treats a prompt title as the earliest point
   * the "✓ / ✗ exit N" marker can settle — a failing builtin like `false`
   * never moves the foreground comm — so swallowing it here would leave the
   * statusline stale until the next poll. */
  g_signal_emit(t, signals[SIG_TITLE_CHANGED], 0, title);
}

/* A program in this pane asked the desktop to say something (OSC 9 / OSC 777).
 * The core has already thrown out everything that should not get this far —
 * the ConEmu extensions sharing OSC 9, notifications from a pane the user is
 * looking at, text that is not UTF-8, and anything over the rate limit — so
 * this only has to say which pane it was. */
static void core_notification(PtTermCore *core, const char *title,
                              const char *body, gpointer user) {
  (void)core;
  PtTerminal *t = PT_TERMINAL(user);
  g_signal_emit(t, signals[SIG_NOTIFICATION], 0, title, body);
}

static void core_command(PtTermCore *core, const char *comm, gpointer user) {
  (void)core;
  PtTerminal *t = PT_TERMINAL(user);
  g_free(t->last_command);
  t->last_command = g_strdup(comm);
  g_signal_emit(t, signals[SIG_COMMAND_CHANGED], 0, comm);
}

/* ---- clipboard writes from programs (OSC 52) ----
 *
 * The core has already decoded the base64, capped the size and thrown out
 * anything malformed, so what is left is where the text goes and whether to
 * ask first. It deliberately does *not* go through the paste sanitizer on the
 * way in: this text is headed for the clipboard, not for a pty, and the
 * newlines and tabs a sanitizer would flatten are the whole point of a yank.
 * The sanitizing belongs at the other end, and is already there — whenever the
 * text comes back out (⌃⇧V, or into any other app), pt_term_core_paste()
 * rewrites the control bytes and pt_term_core_paste_is_safe() asks first, the
 * same as for anything else on the system clipboard. */
static void clipboard_set(PtTerminal *t, const char *text, gboolean primary) {
  GdkClipboard *cb = primary ? gtk_widget_get_primary_clipboard(GTK_WIDGET(t))
                             : gtk_widget_get_clipboard(GTK_WIDGET(t));
  gdk_clipboard_set_text(cb, text);
}

/* Clipboard text held across the confirmation dialog. */
typedef struct {
  PtTerminal *term;   /* owned ref */
  char *text;         /* owned */
  gboolean primary;
  gboolean refocus;   /* the pane had the keyboard when the dialog went up */
} PtOsc52Ctx;

/* On finalize, so it covers the responses and dismissal alike (as with the
 * paste confirmation below). */
static void osc52_ctx_free(gpointer data, GClosure *closure) {
  (void)closure;
  PtOsc52Ctx *p = data;
  p->term->osc52_asking = FALSE;
  g_object_unref(p->term);
  g_free(p->text);
  g_free(p);
}

static void on_osc52_confirm_response(AdwAlertDialog *dlg, const char *response,
                                      gpointer user) {
  (void)dlg;
  PtOsc52Ctx *p = user;
  if (g_strcmp0(response, "copy") == 0)
    clipboard_set(p->term, p->text, p->primary);
  /* Only back to the pane that asked if that is where the keyboard already
   * was. Unlike a paste, nobody started this: the program did, possibly in a
   * pane the user is not looking at, and answering its dialog must not move
   * the next thing they type into a different shell. */
  if (p->refocus && gtk_widget_get_root(GTK_WIDGET(p->term)) != NULL)
    gtk_widget_grab_focus(GTK_WIDGET(p->term));
}

/* Takes ownership of `text` and of `t`'s asking slot; holds `t` alive itself,
 * since a pane can be closed while its dialog is up. */
static void present_osc52_confirm(PtTerminal *t, char *text, gsize len,
                                  gboolean primary) {
  /* Name the pane by whatever is running in it: with several panes open, "a
   * program" is not enough to decide with. */
  const char *who = t->last_command != NULL ? t->last_command : "this pane";
  char *heading = g_strdup_printf("Let %s copy %" G_GSIZE_FORMAT " %s?",
                                  who, len, len == 1 ? "byte" : "bytes");
  char *body = g_strdup_printf(
      "The text goes on the %s, replacing what is there now.",
      primary ? "primary selection" : "clipboard");
  AdwDialog *dlg = adw_alert_dialog_new(heading, body);
  g_free(heading);
  g_free(body);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dlg),
                                 "cancel", "Cancel", "copy", "Copy", NULL);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dlg), "cancel");
  PtOsc52Ctx *p = g_new0(PtOsc52Ctx, 1);
  p->term = g_object_ref(t);
  p->text = text;
  p->primary = primary;
  /* Asked now, while it still means something: presenting the dialog takes
   * the keyboard away from whatever had it. */
  p->refocus = gtk_widget_has_focus(GTK_WIDGET(t));
  g_signal_connect_data(dlg, "response", G_CALLBACK(on_osc52_confirm_response),
                        p, osc52_ctx_free, 0);
  adw_dialog_present(dlg, GTK_WIDGET(t));
}

static void core_clipboard_write(PtTermCore *core, const char *text, gsize len,
                                 gboolean primary, gpointer user) {
  (void)core;
  PtTerminal *t = PT_TERMINAL(user);
  if (t->osc52 == PT_OSC52_OFF) return;
  if (t->osc52 != PT_OSC52_ASK) {
    clipboard_set(t, text, primary);
    return;
  }
  /* A program copying in a loop must not be able to stack dialogs, and a pane
   * with no window has nowhere to put one. Both drop the write rather than
   * take it: nothing reaches the clipboard without an answer. */
  if (t->osc52_asking || gtk_widget_get_root(GTK_WIDGET(t)) == NULL) return;
  t->osc52_asking = TRUE;
  present_osc52_confirm(t, g_strndup(text, len), len, primary);
}

/* ---- glyph cache ----
 *
 * Shaping is the expensive half of drawing text — itemization, font selection
 * and HarfBuzz, all of it per call — and a terminal draws the same few hundred
 * characters over and over. So each (style, grapheme) is shaped once and the
 * result kept for the life of the process, keyed by a small stack-built string.
 *
 * Every glyph's advance is forced to the cell grid. That is what lets adjacent
 * cells share a single text node: the font's natural advance is not exactly
 * cell_w after rounding, so concatenating text would drift a fraction of a
 * pixel per column and visibly bow a long line. Overriding the geometry keeps
 * every cell nailed to its column while still batching. */
typedef struct {
  PangoFont *font;            /* owned ref; runs break when this changes */
  PangoGlyphString *glyphs;   /* owned; advances already snapped to the grid */
} GlyphEntry;

/* Two tiers. The hot one is a direct-mapped array over (bold|italic,
 * codepoint) for single-codepoint cells below U+0300 — ASCII, Latin-1 and
 * Latin Extended, which is nearly every cell a terminal ever draws — so the
 * per-cell lookup is an index, not a hash of a stack-built key. Everything
 * else (clusters, and codepoints past the table) goes to the hash, keyed by
 * the FULL cluster: PT_CELL_TEXT_MAX bounds the cluster at 63 bytes, and the
 * key buffer holds style byte + 63 + NUL, so two clusters sharing a long
 * prefix can no longer collide into one entry. */
#define PT_GLYPH_DIRECT_MAX 0x300
static GlyphEntry *glyph_direct[4][PT_GLYPH_DIRECT_MAX];
static GHashTable *glyph_cache;   /* char key -> GlyphEntry (rare clusters) */

static void glyph_entry_free(gpointer p) {
  GlyphEntry *e = p;
  g_clear_object(&e->font);
  g_clear_pointer(&e->glyphs, pango_glyph_string_free);
  g_free(e);
}

static void glyph_cache_clear(void) {
  if (glyph_cache != NULL) g_hash_table_remove_all(glyph_cache);
  for (guint s = 0; s < 4; s++)
    for (guint cp = 0; cp < PT_GLYPH_DIRECT_MAX; cp++)
      g_clear_pointer(&glyph_direct[s][cp], glyph_entry_free);
}

/* key: one style byte (kept printable so the whole thing is a C string) then
 * the cluster's UTF-8 bytes, whole. */
static void glyph_key(char *out, gsize out_len, const char *utf8, gsize len,
                      gboolean bold, gboolean italic) {
  out[0] = (char)('a' + (bold ? 1 : 0) + (italic ? 2 : 0));
  gsize n = MIN(len, out_len - 2);
  memcpy(out + 1, utf8, n);
  out[1 + n] = '\0';
}

/* Shape one cluster. The font description rides in on the attribute list so
 * itemization picks the terminal font (and its fallbacks) rather than the
 * widget context's default. Always returns an entry; font == NULL marks a
 * cluster nothing can draw, cached so the failure is paid once too. */
static GlyphEntry *glyph_shape(PtTerminal *t, const char *utf8, gsize len,
                               gboolean bold, gboolean italic) {
  PangoContext *pc = gtk_widget_get_pango_context(GTK_WIDGET(t));
  PangoAttrList *attrs = pango_attr_list_new();
  pango_attr_list_insert(attrs, pango_attr_font_desc_new(t->font_desc));
  if (bold)
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
  if (italic)
    pango_attr_list_insert(attrs, pango_attr_style_new(PANGO_STYLE_ITALIC));

  GlyphEntry *e = g_new0(GlyphEntry, 1);
  GList *items = pango_itemize(pc, utf8, 0, (int)len, attrs, NULL);
  if (items != NULL) {
    PangoItem *item = items->data;          /* one cell is one item in practice */
    PangoGlyphString *gs = pango_glyph_string_new();
    pango_shape(utf8, (int)len, &item->analysis, gs);
    /* First glyph carries the cell advance; combining marks stay at zero. */
    for (int i = 0; i < gs->num_glyphs; i++)
      gs->glyphs[i].geometry.width = i == 0 ? t->cell_w * PANGO_SCALE : 0;
    e->font = g_object_ref(item->analysis.font);
    e->glyphs = gs;
    g_list_free_full(items, (GDestroyNotify)pango_item_free);
  }
  pango_attr_list_unref(attrs);
  return e;
}

/* `cp` and `single` say whether text is one codepoint and which — the caller
 * has already decoded it for the block-glyph test, so it is not re-derived. */
static const GlyphEntry *glyph_lookup(PtTerminal *t, const char *utf8,
                                      gsize len, gunichar cp, gboolean single,
                                      gboolean bold, gboolean italic) {
  if (single && cp < PT_GLYPH_DIRECT_MAX) {
    guint s = (bold ? 1u : 0u) | (italic ? 2u : 0u);
    GlyphEntry *e = glyph_direct[s][cp];
    if (e == NULL) {
      e = glyph_shape(t, utf8, len, bold, italic);
      glyph_direct[s][cp] = e;
    }
    return e->font != NULL ? e : NULL;
  }

  char key[2 + PT_CELL_TEXT_MAX];   /* style + whole cluster + NUL, always */
  glyph_key(key, sizeof(key), utf8, len, bold, italic);
  if (glyph_cache == NULL)
    glyph_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        glyph_entry_free);
  GlyphEntry *e = g_hash_table_lookup(glyph_cache, key);
  if (e == NULL) {
    e = glyph_shape(t, utf8, len, bold, italic);
    g_hash_table_insert(glyph_cache, g_strdup(key), e);
  }
  return e->font != NULL ? e : NULL;
}

/* Emit one text node for the accumulated run and reset it. */
static void flush_run(GtkSnapshot *snapshot, PangoFont *font,
                      PangoGlyphString *run, const GdkRGBA *color,
                      int x, int y, int baseline) {
  if (run->num_glyphs == 0) return;
  if (font != NULL) {
    GskRenderNode *node = gsk_text_node_new(
        font, run, color, &GRAPHENE_POINT_INIT(x, y + baseline));
    if (node != NULL) {
      gtk_snapshot_append_node(snapshot, node);
      gsk_render_node_unref(node);
    }
  }
  run->num_glyphs = 0;
}

/* ---- decorations ----
 *
 * Everything drawn from a cell's style bits and its underline field: SGR
 * underline (five shapes), overline, strikethrough, and the pt-only link
 * underline. Split into two passes on either side of the glyph pass, in the
 * same order ghostty's renderer composites them in (generic.zig:2946-2963
 * for underline/overline, :3042-3048 for strikethrough) even though ghostty
 * writes the per-cell code for all of it in one x-loop — its background,
 * underline/overline, glyph and strikethrough cells land in separate GPU
 * buffers that get drawn in that fixed order regardless of loop order, so
 * the *loop* order there is not the *paint* order. GTK has no such split:
 * a GskRenderNode snapshot is painted in the order it was appended, so pt
 * has to walk the row twice more (once before the glyphs, once after) to
 * get the same z-order ghostty gets for free. Ghostty's stated reason is
 * readability: "We draw underlines first so that they layer underneath
 * text. This improves readability when a colored underline is used which
 * intersects parts of the text (descenders)" (generic.zig:2946-2948) — an
 * underline sits close enough to the baseline that a g, y, p, q or j's
 * descender reaches into it, and a solid colored bar painted on top of that
 * descender reads worse than one painted under it. Strikethrough has no such
 * problem (it crosses the letter's body, not a descender) and ghostty draws
 * it after the glyph too, so pt matches that as well rather than inventing
 * a different rule for it.
 *
 * draw_row_underline_overline runs right after the row's backgrounds, before
 * any glyph in the row is drawn, and is also where pt's link underline is
 * decided now (see below). draw_row_strike runs after the row's glyphs and
 * draws strikethrough only.
 *
 * Colour: an SGR underline uses `underline_color` when the cell set one
 * (SGR 58), otherwise the cell's own resolved foreground, same as ghostty
 * (generic.zig:2951, `style.underlineColor(&state.colors.palette) orelse
 * fg`). This applies whether the underline came from the cell's own SGR 4 or
 * from the link merge below — ghostty's colour lookup runs after the merge
 * and does not care which one produced it. Strikethrough and overline always
 * use the resolved foreground — ghostty never lets SGR 58 touch them; its
 * addOverline and addStrikethrough calls both pass `fg`, not the underline
 * colour. Faint multiplies every decoration's alpha by the same factor the
 * glyph gets: 0.5, ghostty's `faint-opacity` default (config/Config.zig:
 * 3716), applied at all four call sites in generic.zig (:2954, :2962, :3033,
 * :3048). pt has no setting for this, so 0.5 is a literal, matched to the
 * reference implementation.
 *
 * Invisible and the link merge, in ghostty's order (generic.zig:2920-2944):
 * the `continue` for an invisible cell (:2920-2930) runs before the link
 * merge that decides whether a cell gets a link's underline (:2930-2944), so
 * an invisible cell never reaches the merge at all and draws no underline,
 * link or SGR, same as it draws no glyph, overline or strikethrough. pt
 * matches that by gating `is_link` on `!invisible` before it ever asks
 * whether the cell is a link.
 *
 * Blink is not read here at all: it hides the glyph itself (see the glyph
 * loop), not what decorates it. A blinking, underlined word keeps its
 * underline solid through the off phase, the way most terminals that blink
 * text at all still leave the line under it alone.
 *
 * Link underline: ghostty never draws two underlines on one cell
 * (generic.zig:2930-2944, `Give links a single underline, unless they
 * already have an underline, in which case use a double underline to
 * distinguish them`). pt used to keep its own separate 1px link underline
 * and draw it in addition to any SGR underline, which could put two lines
 * under one linked, underlined cell; that was wrong and pt now follows
 * ghostty's rule exactly. The decision is made once per cell, before
 * anything about that cell is drawn: if the cell is a link and its SGR
 * underline is SINGLE, draw DOUBLE; if it is a link in any other SGR state
 * (including none), draw SINGLE; if it is not a link, draw its own SGR
 * underline unchanged. There is no longer a separate link pass or a
 * separate link geometry — a link's underline is an ordinary underline at
 * the SGR geometry's `underline_y`, merged into the same run machinery as
 * everything else below.
 *
 * Two kinds of link feed that decision. An OSC 8 cell counts as a link
 * whenever it is on screen, because the program declared it. A bare URL
 * counts as a link only over the columns [span_a, span_b] of this row, and
 * only while it is the one under the pointer with the modifier down —
 * ghostty highlights its regex links on hover too, for the same reason:
 * every address in a build log underlined at once is noise, not
 * information. span_a > span_b means this row has no such span.
 *
 * Runs: single and double underline, and overline, are plain rects, so
 * adjacent cells that agree on shape and colour merge into one rect, the
 * way the background pass merges adjacent cells (:882-919 above).
 * Strikethrough merges the same way in the second pass. Curly, dotted and
 * dashed do not merge: each is a periodic shape whose period ghostty ties to
 * a single cell width by drawing it from a per-cell sprite and tiling that
 * sprite at every column (font/sprite/draw/special.zig) — dashed included,
 * since its dash_width and dash_count are computed from the sprite's own
 * cell-width canvas, not from however long the underlined run turns out to
 * be. An earlier version of this pass merged dashed with single and double,
 * on the theory that only curly and dotted needed the per-cell restart; that
 * was wrong; computing dash_width from a merged run's width scales the dash
 * period with the length of the word, which is not what a dashed underline
 * looks like anywhere else, ghostty included. So dashed joins curly and
 * dotted in drawing once per cell regardless of what its neighbours are
 * doing. */

static void draw_rect(GtkSnapshot *snapshot, float x, float y, float w,
                      float h, const GdkRGBA *color) {
  gtk_snapshot_append_color(snapshot, color, &GRAPHENE_RECT_INIT(x, y, w, h));
}

/* One cycle of a sine-shaped wave per cell, geometry matched to ghostty's
 * underline_curly (font/sprite/draw/special.zig:162-235): amplitude = cell
 * width / pi, two cubic beziers meeting at the cell's centre with 0.4
 * curvature, round caps, stroke width = underline_thickness. `top` is
 * clamped so the wave cannot draw below the row it belongs to — the same job
 * ghostty's canvas clamp does for its sprite. */
static void draw_underline_curly(PtTerminal *t, GtkSnapshot *snapshot,
                                 int x, int y, const GdkRGBA *color) {
  double w = t->cell_w;
  double th = t->underline_thickness;
  double amplitude = w / G_PI;
  double top_max = MAX(0.0, t->cell_h - amplitude - th);
  double top = MIN((double)t->underline_y, top_max);
  double bottom = top + amplitude;
  double center = 0.5 * w;
  double r = 0.4;

  GskPathBuilder *pb = gsk_path_builder_new();
  gsk_path_builder_move_to(pb, (float)x, (float)(y + bottom));
  gsk_path_builder_cubic_to(pb,
      (float)(x + center * r), (float)(y + bottom),
      (float)(x + center - center * r), (float)(y + top),
      (float)(x + center), (float)(y + top));
  gsk_path_builder_cubic_to(pb,
      (float)(x + center + center * r), (float)(y + top),
      (float)(x + w - center * r), (float)(y + bottom),
      (float)(x + w), (float)(y + bottom));
  GskPath *path = gsk_path_builder_free_to_path(pb);
  GskStroke *stroke = gsk_stroke_new((float)th);
  gsk_stroke_set_line_cap(stroke, GSK_LINE_CAP_ROUND);
  gtk_snapshot_append_stroke(snapshot, path, stroke, color);
  gsk_stroke_free(stroke);
  gsk_path_unref(path);
}

/* A row of filled circles, radius and spacing matched to ghostty's
 * underline_dotted (font/sprite/draw/special.zig:72-129): radius =
 * sqrt(1/2) * underline_thickness (a circle at the underline's own thickness
 * looks anemic), dot count is the largest value that keeps the gaps no
 * smaller than a dot's radius and no larger than its diameter, floored at
 * one dot per cell, and the cell is split into that many equal slots with
 * one dot centred in each. */
static void draw_underline_dotted(PtTerminal *t, GtkSnapshot *snapshot,
                                  int x, int y, const GdkRGBA *color) {
  double w = t->cell_w;
  double th = t->underline_thickness;
  double radius = (G_SQRT2 / 2.0) * th;
  double cy = MIN(t->underline_y + 0.5 * th, t->cell_h - ceil(radius));
  if (cy < 0) cy = 0;
  double count = MAX(1.0, MIN(MIN(ceil(w / (4 * radius)),
                                  floor(w / (3 * radius))),
                              floor(w / (2 * radius + 1))));
  double slot = w / count;
  double cx = slot / 2;

  GskPathBuilder *pb = gsk_path_builder_new();
  for (int i = 0; i < (int)count; i++) {
    gsk_path_builder_add_circle(pb,
        &GRAPHENE_POINT_INIT((float)(x + cx), (float)(y + cy)), (float)radius);
    cx += slot;
  }
  GskPath *path = gsk_path_builder_free_to_path(pb);
  gtk_snapshot_append_fill(snapshot, path, GSK_FILL_RULE_WINDING, color);
  gsk_path_unref(path);
}

/* One cell's worth of dashes, dash_width and dash_count from special.zig:
 * 131-160, computed from this single cell's width so the period matches
 * ghostty's per-cell sprite exactly — see the header comment above for why
 * this is not merged into a wider run the way single/double/overline are. */
static void draw_underline_dashed(PtTerminal *t, GtkSnapshot *snapshot,
                                  int x, int y, const GdkRGBA *color) {
  int w = t->cell_w;
  int dash_w = w / 3 + 1;
  int dash_count = w / dash_w + 1;
  for (int i = 0; i < dash_count; i += 2)
    draw_rect(snapshot, x + i * dash_w, y + t->underline_y, dash_w,
              t->underline_thickness, color);
}

/* Which straight-rect decoration a flushed run is: underline (only single or
 * double ever reach here now — curly, dotted and dashed are drawn per-cell,
 * see above), strikethrough or overline. The three share the merge-and-flush
 * shape below but draw at different y-offsets. */
typedef enum { PT_DECO_UNDERLINE, PT_DECO_STRIKE, PT_DECO_OVERLINE } PtDecoKind;

static void draw_straight_run(PtTerminal *t, GtkSnapshot *snapshot,
                              PtDecoKind kind, PtUnderline style,
                              int x, int y, int w, const GdkRGBA *color) {
  if (kind == PT_DECO_STRIKE) {
    draw_rect(snapshot, x, y + t->strike_y, w, t->strike_thickness, color);
    return;
  }
  if (kind == PT_DECO_OVERLINE) {
    /* Ghostty pins overline to the top of the cell with the underline's own
     * thickness (Metrics.zig:298-318: overline_position = 0, overline_
     * thickness = underline_thickness), so there is no separate field for
     * it to draw from. */
    draw_rect(snapshot, x, y, w, t->underline_thickness, color);
    return;
  }
  switch (style) {
    case PT_UNDERLINE_SINGLE:
      draw_rect(snapshot, x, y + t->underline_y, w, t->underline_thickness, color);
      break;
    case PT_UNDERLINE_DOUBLE: {
      /* Two rects at underline_y ± one thickness; the row the single
       * underline would occupy is left empty between them (special.zig:
       * 38-70). Each offset is clamped into the row on its own — going one
       * thickness either side of an already-clamped underline_y can still
       * walk off the top or bottom for a font with an extreme metric. */
      int th = t->underline_thickness;
      int top_max = MAX(0, t->cell_h - th);
      int y0 = CLAMP(t->underline_y - th, 0, top_max);
      int y1 = CLAMP(t->underline_y + th, 0, top_max);
      draw_rect(snapshot, x, y + y0, w, th, color);
      draw_rect(snapshot, x, y + y1, w, th, color);
      break;
    }
    default: break;   /* NONE/CURLY/DOTTED/DASHED never reach a run */
  }
}

/* First of the two decoration passes: SGR underline and overline, drawn
 * before this row's glyphs so a colored underline sits under a descender
 * rather than over it (see the header comment). Backgrounds are already
 * down by the time this runs. This is also where a link's underline is
 * decided, folded into the same per-cell u_kind so a cell never ends up
 * with two underlines (see the header comment's "Link underline"
 * paragraph). span_a/span_b are the bare-URL hover span's columns on this
 * row; span_a > span_b means there is none. */
static void draw_row_underline_overline(PtTerminal *t, GtkSnapshot *snapshot,
                                        const PtCell *cells, int n, int y,
                                        PtColor bg_default, int span_a,
                                        int span_b) {
  gboolean u_open = FALSE;
  PtUnderline u_style = PT_UNDERLINE_NONE;
  GdkRGBA u_color = { 0, 0, 0, 0 };
  int u_x = 0, u_w = 0;

  gboolean o_open = FALSE;
  GdkRGBA o_color = { 0, 0, 0, 0 };
  int o_x = 0, o_w = 0;

  int x = pad_x;
  for (int i = 0; i < n; i++, x += t->cell_w) {
    const PtCell *cl = &cells[i];
    gboolean invisible = (cl->style & PT_CELL_STYLE_INVISIBLE) != 0;
    PtColor fg = cl->fg;
    if (cl->style & PT_CELL_STYLE_INVERSE)
      fg = cl->has_bg ? cl->bg : bg_default;
    float alpha = (cl->style & PT_CELL_STYLE_FAINT) ? 0.5f : 1.0f;
    GdkRGBA fg_rgba = { fg.r / 255.0f, fg.g / 255.0f, fg.b / 255.0f, alpha };

    /* Link merge (generic.zig:2930-2944): a link cell that already had a
     * single underline gets a double one to stand out from it; a link cell
     * in any other SGR state, including none, gets a single one; a
     * non-link cell keeps its own SGR underline untouched. Gated on
     * !invisible first, matching ghostty's `continue` at :2920-2930 firing
     * before the merge ever runs, so an invisible cell never becomes a
     * link underline either. */
    gboolean is_link = !invisible &&
        (cl->has_link || (i >= span_a && i <= span_b));
    PtUnderline sgr_kind = invisible ? PT_UNDERLINE_NONE : (PtUnderline)cl->underline;
    PtUnderline u_kind = is_link
        ? (sgr_kind == PT_UNDERLINE_SINGLE ? PT_UNDERLINE_DOUBLE : PT_UNDERLINE_SINGLE)
        : sgr_kind;
    GdkRGBA u_want = fg_rgba;
    if (u_kind != PT_UNDERLINE_NONE && cl->has_underline_color) {
      PtColor uc = cl->underline_color;
      u_want = (GdkRGBA){ uc.r / 255.0f, uc.g / 255.0f, uc.b / 255.0f, alpha };
    }

    if (u_kind == PT_UNDERLINE_CURLY || u_kind == PT_UNDERLINE_DOTTED ||
        u_kind == PT_UNDERLINE_DASHED) {
      if (u_open) {
        draw_straight_run(t, snapshot, PT_DECO_UNDERLINE, u_style, u_x, y, u_w, &u_color);
        u_open = FALSE;
      }
      if (u_kind == PT_UNDERLINE_CURLY)
        draw_underline_curly(t, snapshot, x, y, &u_want);
      else if (u_kind == PT_UNDERLINE_DOTTED)
        draw_underline_dotted(t, snapshot, x, y, &u_want);
      else
        draw_underline_dashed(t, snapshot, x, y, &u_want);
    } else {
      if (u_open && (u_kind != u_style || !gdk_rgba_equal(&u_want, &u_color))) {
        draw_straight_run(t, snapshot, PT_DECO_UNDERLINE, u_style, u_x, y, u_w, &u_color);
        u_open = FALSE;
      }
      if (u_kind != PT_UNDERLINE_NONE) {
        if (!u_open) { u_open = TRUE; u_style = u_kind; u_color = u_want; u_x = x; u_w = t->cell_w; }
        else u_w += t->cell_w;
      }
    }

    /* overline: its own run, keyed on its own on/off state and the resolved
     * foreground alone — it never takes the underline colour, see the
     * header comment above. */
    gboolean want_overline = !invisible && (cl->style & PT_CELL_STYLE_OVERLINE);
    if (o_open && (!want_overline || !gdk_rgba_equal(&fg_rgba, &o_color))) {
      draw_straight_run(t, snapshot, PT_DECO_OVERLINE, PT_UNDERLINE_NONE, o_x, y, o_w, &o_color);
      o_open = FALSE;
    }
    if (want_overline) {
      if (!o_open) { o_open = TRUE; o_color = fg_rgba; o_x = x; o_w = t->cell_w; }
      else o_w += t->cell_w;
    }
  }

  if (u_open) draw_straight_run(t, snapshot, PT_DECO_UNDERLINE, u_style, u_x, y, u_w, &u_color);
  if (o_open) draw_straight_run(t, snapshot, PT_DECO_OVERLINE, PT_UNDERLINE_NONE, o_x, y, o_w, &o_color);
}

/* Second of the two decoration passes: strikethrough, drawn after this row's
 * glyphs the same way ghostty draws it (generic.zig:3042-3048, after the
 * glyph call in its own per-cell loop) — a strikethrough crosses the body of
 * a letter rather than reaching into a descender, so there is no readability
 * reason to put it under the glyph the way underline/overline are. The link
 * underline no longer has a pass of its own; it is folded into the SGR
 * underline decision in draw_row_underline_overline above. */
static void draw_row_strike(PtTerminal *t, GtkSnapshot *snapshot,
                            const PtCell *cells, int n, int y,
                            PtColor bg_default) {
  gboolean s_open = FALSE;
  GdkRGBA s_color = { 0, 0, 0, 0 };
  int s_x = 0, s_w = 0;

  int x = pad_x;
  for (int i = 0; i < n; i++, x += t->cell_w) {
    const PtCell *cl = &cells[i];
    gboolean invisible = (cl->style & PT_CELL_STYLE_INVISIBLE) != 0;
    PtColor fg = cl->fg;
    if (cl->style & PT_CELL_STYLE_INVERSE)
      fg = cl->has_bg ? cl->bg : bg_default;
    float alpha = (cl->style & PT_CELL_STYLE_FAINT) ? 0.5f : 1.0f;
    GdkRGBA fg_rgba = { fg.r / 255.0f, fg.g / 255.0f, fg.b / 255.0f, alpha };

    gboolean want_strike = !invisible && (cl->style & PT_CELL_STYLE_STRIKE);
    if (s_open && (!want_strike || !gdk_rgba_equal(&fg_rgba, &s_color))) {
      draw_straight_run(t, snapshot, PT_DECO_STRIKE, PT_UNDERLINE_NONE, s_x, y, s_w, &s_color);
      s_open = FALSE;
    }
    if (want_strike) {
      if (!s_open) { s_open = TRUE; s_color = fg_rgba; s_x = x; s_w = t->cell_w; }
      else s_w += t->cell_w;
    }
  }

  if (s_open) draw_straight_run(t, snapshot, PT_DECO_STRIKE, PT_UNDERLINE_NONE, s_x, y, s_w, &s_color);
}

/* ---- geometry ---- */
static void measure_font(PtTerminal *t) {
  PangoContext *pc = gtk_widget_get_pango_context(GTK_WIDGET(t));
  PangoFontMetrics *m =
      pango_context_get_metrics(pc, t->font_desc, NULL);
  t->cell_w = PANGO_PIXELS(pango_font_metrics_get_approximate_digit_width(m));
  t->cell_h = PANGO_PIXELS(pango_font_metrics_get_ascent(m) +
                           pango_font_metrics_get_descent(m));
  /* Text nodes are positioned by baseline, not by the layout's top-left the
   * way gtk_snapshot_append_layout was. */
  t->baseline = PANGO_PIXELS(pango_font_metrics_get_ascent(m));

  /* Underline/strikethrough geometry, matched to ghostty's own formulas
   * (build/_deps/ghostty-src/src/font/Metrics.zig:298-318): thickness is the
   * face's own thickness rounded up with a 1px floor, position is the face's
   * own distance from the baseline rounded to the nearest pixel. Pango
   * reports both distances in font units with "above the baseline" positive,
   * so baseline - PANGO_PIXELS(position) is ghostty's
   * top_to_baseline - facePosition(). Overline has no face metric of its
   * own — ghostty pins it to the top of the cell and reuses the underline's
   * own thickness (same Metrics.zig lines) — so the decoration pass below
   * draws it at y=0 with underline_thickness rather than a field of its own.
   * Measured once per font change, same as the rest of this function, rather
   * than once per frame. */
  t->underline_thickness = (int)ceil(
      pango_font_metrics_get_underline_thickness(m) / (double)PANGO_SCALE);
  t->underline_y = t->baseline - (int)lround(
      pango_font_metrics_get_underline_position(m) / (double)PANGO_SCALE);
  t->strike_thickness = (int)ceil(
      pango_font_metrics_get_strikethrough_thickness(m) / (double)PANGO_SCALE);
  t->strike_y = t->baseline - (int)lround(
      pango_font_metrics_get_strikethrough_position(m) / (double)PANGO_SCALE);

  pango_font_metrics_unref(m);
  if (t->cell_w < 1) t->cell_w = 8;
  if (t->cell_h < 1) t->cell_h = 16;
  if (t->baseline < 1) t->baseline = t->cell_h;
  /* Same defensive floor as cell_w/cell_h/baseline above: a font reporting
   * garbage metrics must not hand the decoration pass a zero-or-negative
   * thickness, or a y that has walked off the row the glyph itself sits in. */
  if (t->underline_thickness < 1) t->underline_thickness = 1;
  if (t->strike_thickness < 1) t->strike_thickness = 1;
  t->underline_y = CLAMP(t->underline_y, 0,
                         MAX(0, t->cell_h - t->underline_thickness));
  t->strike_y = CLAMP(t->strike_y, 0, MAX(0, t->cell_h - t->strike_thickness));
  /* Cached advances are in terms of cell_w, and the cached font follows the
   * description — both just changed. */
  glyph_cache_clear();
}

/* Theme colors pushed into the core: the ANSI slots the theme pins (so status
 * output matches the app chrome) plus the default bg/fg/cursor. The core's
 * pinned-slot encoding is alpha > 0, so the alpha is set from th_pal_set here
 * rather than trusted from the theme struct. */
static void apply_palette(PtTermCore *core) {
  PtTermColors colors = { .bg = th_bg, .fg = th_fg, .cursor = th_cursor };
  for (int i = 0; i < 16; i++) {
    colors.palette[i] = th_pal[i];
    colors.palette[i].a = th_pal_set[i] ? 1.0 : 0.0;
  }
  pt_term_core_set_colors(core, &colors);
}

/* The grid an allocation of `width` x `height` pixels holds, at the current
 * cell metrics. Two is the floor: a one-column terminal is not a terminal. */
static void grid_for_size(PtTerminal *t, int width, int height,
                          int *cols, int *rows) {
  *cols = (width - 2 * pad_x) / t->cell_w;
  *rows = (height - 2 * pad_y) / t->cell_h;
  if (*cols < 2) *cols = 2;
  if (*rows < 2) *rows = 2;
}

static void ensure_core(PtTerminal *t) {
  if (t->core != NULL) return;
  measure_font(t);
  /* Spawn at the size the pane already has, so the child's very first winsize
   * is the real one: a shell that prints a prompt or an app that draws before
   * pt's first resize lands would otherwise lay itself out for 80x24 and wrap
   * against a grid that was never there. A widget with no allocation yet (the
   * core built from the reset path, ahead of the queued allocate) has no size
   * to use and keeps the 80x24 default; the size-allocate resize below still
   * corrects it, SIGWINCH and all. */
  int width = gtk_widget_get_width(GTK_WIDGET(t));
  int height = gtk_widget_get_height(GTK_WIDGET(t));
  int cols = 80, rows = 24;
  if (width > 0 && height > 0) grid_for_size(t, width, height, &cols, &rows);
  GError *err = NULL;
  t->core = pt_term_core_new(t->start_cwd, NULL,
                             (const char *const *)t->env,
                             (guint16)cols, (guint16)rows,
                             t->cell_w, t->cell_h, scrollback_limit, &err);
  if (t->core == NULL) {
    g_warning("pt: terminal spawn failed: %s",
              err != NULL ? err->message : "?");
    g_clear_error(&err);
    t->exited = TRUE;
    t->exit_status = -1;
    return;
  }
  apply_palette(t->core);
  /* Registering clipboard_write or notification is what starts the OSC
   * scanner: with no consumer at all the core does not even look at the
   * bytes. */
  PtTermCoreCallbacks cbs = { .draw = core_draw, .output = core_output,
                              .exited = core_exited,
                              .title = core_title, .command = core_command,
                              .clipboard_write = core_clipboard_write,
                              .notification = core_notification };
  pt_term_core_set_callbacks(t->core, &cbs, t);
  pt_term_core_set_osc52(t->core, t->osc52);
  /* Same reason as the scheme below: a pane spawned after a config change has
   * to start on the inset the widget is already drawing with, or its first
   * click lands a cell away from where it looks. */
  pt_term_core_set_padding(t->core, pad_x, pad_y);
  /* Cores are built lazily, long after the theme was applied globally, so the
   * scheme has to be seeded here or a pane opened later answers 996 with the
   * default instead of the active theme. Nothing is written: the child has not
   * run a byte yet, so mode 2031 cannot be on. */
  pt_term_core_set_color_scheme(t->core, th_dark);
  /* The core is built lazily from size-allocate, which is later than the focus
   * grab in show_active_grid: on a new tab the first focus-in has already come
   * and gone by now. Push the widget's real state in so the core does not start
   * out believing a focused pane is unfocused. */
  pt_term_core_focus_report(t->core, t->focused, FALSE);
  if (t->startup_input != NULL) {
    /* Restore's resume command. After the spawn and before anything else the
     * user could type: the pty queues it until the shell is ready, so it
     * lands as the first line at the first prompt. One-shot — a respawn from
     * restart_shell must give a plain shell, not a surprise second resume.
     * Only reached on a spawn that succeeded: the failure path above returns
     * early and leaves the line for the retry a restart_shell would be. */
    pt_term_core_write(t->core, t->startup_input, -1);
    g_clear_pointer(&t->startup_input, g_free);
  }
}

static void pt_terminal_size_allocate(GtkWidget *widget, int width, int height,
                                      int baseline) {
  (void)baseline;
  PtTerminal *t = PT_TERMINAL(widget);
  ensure_core(t);
  if (t->core == NULL) return;
  int cols, rows;
  grid_for_size(t, width, height, &cols, &rows);
  pt_term_core_resize(t->core, (guint16)cols, (guint16)rows,
                      t->cell_w, t->cell_h);
  /* Reflow can carry a link away from a pointer that never moved, and a pane
   * that only ever gets resized (a split, a font change) sees no output to
   * catch it on. */
  update_link_cursor(t);
}

/* ---- overlay scrollbar ----
 *
 * How opaque the thumb is right now: solid for PT_BAR_HOLD_MS after the last
 * time the viewport moved, then out over PT_BAR_FADE_MS. Zero means there is
 * nothing to draw, which is the resting state of every pane.
 *
 * No ghostty precedent for the drawing: its GTK apprt puts the surface inside
 * a GtkScrolledWindow and lets GTK render the overlay indicator
 * (apprt/gtk/class/surface.zig:984). pt's terminal is one custom widget that
 * paints its own frame, so the bar is painted with it and the timing is what
 * an overlay indicator does — appear on movement, fade when it stops. */
static double bar_alpha(PtTerminal *t) {
  if (t->bar_at == 0) return 0.0;
  double ms = (double)(g_get_monotonic_time() - t->bar_at) / 1000.0;
  if (ms <= PT_BAR_HOLD_MS) return 1.0;
  double gone = (ms - PT_BAR_HOLD_MS) / PT_BAR_FADE_MS;
  return gone >= 1.0 ? 0.0 : 1.0 - gone;
}

/* The thumb there is to draw, and FALSE when there is none: nothing above the
 * screen to move through, or the alternate screen, where a full-screen app owns
 * the pane and keeps no history behind it (the library gives that screen no
 * scrollback at all, Terminal.zig:2994, so the numbers agree — this is belt and
 * braces rather than a second rule). All three out params are written on TRUE.
 *
 * Asked before the animation starts as well as during it, so a wheel in a pane
 * with no history costs nothing at all. */
static gboolean bar_thumb(PtTerminal *t, guint64 *total, guint64 *offset,
                          guint64 *len) {
  return t->core != NULL && !pt_term_core_alt_screen(t->core) &&
         pt_term_core_scrollbar(t->core, total, offset, len) &&
         *total > *len;
}

static gboolean bar_fade_tick(GtkWidget *widget, GdkFrameClock *clock,
                              gpointer user) {
  (void)clock; (void)user;
  PtTerminal *t = PT_TERMINAL(widget);
  gtk_widget_queue_draw(widget);
  if (bar_alpha(t) > 0.0) return G_SOURCE_CONTINUE;
  /* Faded out: stop asking for frames until something moves again. */
  t->bar_at = 0;
  t->bar_tick = 0;
  return G_SOURCE_REMOVE;
}

/* The hold is over; the fade is the part that needs a frame each. */
static gboolean bar_hold_done(gpointer user) {
  PtTerminal *t = user;
  t->bar_hold = 0;
  if (t->bar_tick == 0)
    t->bar_tick = gtk_widget_add_tick_callback(GTK_WIDGET(t), bar_fade_tick,
                                               NULL, NULL);
  return G_SOURCE_REMOVE;
}

/* The viewport moved: show the bar and restart the fade.
 *
 * A frame callback is only asked for once the hold expires. Nothing changes
 * on screen while the thumb sits at full strength, and a snapshot here rebuilds
 * every glyph run in the pane — a tick callback through the hold would repaint
 * the whole grid tens of times to draw the same pixels. */
static void bar_reveal(PtTerminal *t) {
  guint64 total = 0, offset = 0, len = 0;
  if (!bar_thumb(t, &total, &offset, &len)) return;
  t->bar_at = g_get_monotonic_time();
  if (t->bar_tick != 0) {
    gtk_widget_remove_tick_callback(GTK_WIDGET(t), t->bar_tick);
    t->bar_tick = 0;         /* a fade in progress starts over */
  }
  if (t->bar_hold != 0) g_source_remove(t->bar_hold);
  t->bar_hold = g_timeout_add(PT_BAR_HOLD_MS, bar_hold_done, t);
  gtk_widget_queue_draw(GTK_WIDGET(t));
}

/* ---- rendering ---- */

/* What to actually draw at the cursor, which is not the same question as what
 * the app asked for. */
typedef enum {
  PT_CURSOR_NONE,            /* draw nothing at all */
  PT_CURSOR_BLOCK,
  PT_CURSOR_BLOCK_HOLLOW,
  PT_CURSOR_BAR,
  PT_CURSOR_UNDERLINE,
} PtCursorShape;

/* pt's port of ghostty's renderer/cursor.zig:36-68. The order of the tests is
 * the whole point: it is a priority list of what overrides what, and reading it
 * top to bottom is how you check that exactly one filled cursor can be on
 * screen at a time.
 *
 * Two deliberate departures from ghostty. It has a preedit case above the rest
 * (an IME composing over the cursor forces a block); pt has no IM context at
 * all, so there is no state to test. And where ghostty draws a Nerd Font lock
 * glyph at a password prompt, pt draws a plain block: pt does not ship a font
 * and cannot promise U+F023 exists, and a missing-glyph box at the moment
 * someone is typing a password is the worst place to find out. */
static PtCursorShape cursor_shape(PtTerminal *t, gboolean in_vp,
                                  const PtCursorInfo *ci) {
  /* No shell: the exited banner is up and there is nothing to point at. */
  if (t->exited) return PT_CURSOR_NONE;

  /* Scrolled out of the viewport, or otherwise nowhere to draw. */
  if (!in_vp) return PT_CURSOR_NONE;

  /* A password prompt outranks everything below, hiding and blinking
   * included: whatever else is true, the cursor stays put and stays obvious. */
  if (ci->password) return PT_CURSOR_BLOCK;

  /* The app hid the cursor (DECTCEM). */
  if (!ci->visible) return PT_CURSOR_NONE;

  /* An unfocused pane is hollow whatever it asked for, and never blinks —
   * which is also what makes the focused pane findable in a split. */
  if (!t->focused) return PT_CURSOR_BLOCK_HOLLOW;

  /* Blinking, and this is the off half of the cycle. */
  if (t->blink_wanted && !t->blink_visible) return PT_CURSOR_NONE;

  switch (ci->style) {
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR:
      return PT_CURSOR_BAR;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE:
      return PT_CURSOR_UNDERLINE;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW:
      return PT_CURSOR_BLOCK_HOLLOW;
    default:
      return PT_CURSOR_BLOCK;
  }
}

static void pt_terminal_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  PtTerminal *t = PT_TERMINAL(widget);
  int w = gtk_widget_get_width(widget);
  int h = gtk_widget_get_height(widget);
  gint64 frame_t0 = pt_debug_enabled() ? g_get_monotonic_time() : 0;

  ensure_core(t);
  if (t->core == NULL) {
    gtk_snapshot_append_color(snapshot,
        &(GdkRGBA){th_bg.r / 255.0f, th_bg.g / 255.0f, th_bg.b / 255.0f, 1},
        &GRAPHENE_RECT_INIT(0, 0, w, h));
    return;
  }
  /* Sync only when the core says a frame would differ. Focus, scrim, blink
   * and bar-fade repaints redraw from what the last sync left in place, which
   * is byte-identical — the state only moves when the serial does. The core
   * owns the rule because it also owns the other half of it: those same
   * repaint paths reach a snapshot on their own, without going through
   * core_draw, so this is where a frame held by synchronized output would
   * otherwise leak onto the screen. See pt_term_core_take_frame. */
  if (pt_term_core_take_frame(t->core)) pt_term_core_sync(t->core);

  /* The effective default background: the theme's, unless a program moved it
   * with OSC 11. Seeded so a refused read still paints something sane. */
  PtColor bg_default = th_bg;
  pt_term_core_default_colors(t->core, &bg_default, NULL);

  gtk_snapshot_append_color(snapshot,
      &(GdkRGBA){ bg_default.r / 255.0f, bg_default.g / 255.0f,
                  bg_default.b / 255.0f, 1.0f },
      &GRAPHENE_RECT_INIT(0, 0, w, h));

  /* the theme's selection background takes over the cell background */
  GdkRGBA sel_rgba = { th_sel.r / 255.0f, th_sel.g / 255.0f,
                       th_sel.b / 255.0f, (float)th_sel.a };

  /* One row of flat cells at a time, all rows in a single sequential walk —
   * plain memory from here down, no per-cell FFI. The reader owns the row
   * buffer and sizes it to the row, so every column renders whatever the
   * pane's width. */
  const PtCell *cells;
  /* The run being accumulated: glyphs for consecutive cells that share a font
   * and a colour. Flushed on any change, on a blank, and at end of row. */
  PangoGlyphString *run = pango_glyph_string_new();
  PangoFont *run_font = NULL;
  GdkRGBA run_color = { 0, 0, 0, 1 };
  int run_x = 0;
  int y = pad_y;
  PtRowReader *rows = pt_term_core_rows_begin(t->core);
  int ncells;
  int row_i = -1;               /* the visible row the walk stands on */
  /* Whether any cell in the viewport just walked carries SGR 5 — folded into
   * sync_blink_timer's predicate below once the walk finishes, since that is
   * the earliest point this frame knows the answer. See the text_blink field
   * comment on the struct for why this lives here instead of a core
   * accessor. */
  gboolean text_blink = FALSE;
  while (rows != NULL &&
         (ncells = pt_term_core_rows_next(rows, &cells)) >= 0) {
    row_i++;
    /* The highlighted bare URL's columns on this row, if it reaches it: the
     * whole row between its first and last, and the head or tail on those.
     * Computed here, before the underline/overline pass below, because that
     * pass is now where the link/underline merge happens (see the ----
     * decorations ---- comment's "Link underline" paragraph) and needs to
     * know a cell's link status before it draws anything. */
    int span_a = 0, span_b = -1;
    if (t->url_uri != NULL && row_i >= t->url_r0 && row_i <= t->url_r1) {
      span_a = row_i == t->url_r0 ? t->url_c0 : 0;
      span_b = row_i == t->url_r1 ? t->url_c1 : ncells - 1;
    }
    /* Backgrounds first, merged: adjacent cells with the same effective
     * background (selection included) become one rect, the way adjacent
     * glyphs already share one text node. Laying the whole row's backgrounds
     * down before any of its glyphs is what makes the merge safe — no rect
     * appended here can cover a glyph. */
    /* Whether each decoration pass below has anything at all to do for this
     * row, so a row of plain text (the overwhelming majority in practice)
     * skips a second or third full walk of its cells rather than calling in
     * to find out it had nothing to draw. False positives (a bit set on a
     * cell that turns out invisible) just mean the call happens and draws
     * nothing; only a false negative would be wrong, and there is none here
     * since every bit either pass reads is read here too. Split in two
     * because the two passes now run on opposite sides of the glyph loop —
     * see the ---- decorations ---- comment below. row_has_uo now also
     * covers a link cell, since a link is folded into the underline decision
     * in that pass rather than drawn separately. */
    gboolean row_has_uo = FALSE;     /* underline/overline pass */
    gboolean row_has_strike = FALSE; /* strikethrough pass */
    int bg_x = 0, bg_w = 0;
    GdkRGBA bg_color = { 0, 0, 0, 0 };
    int x = pad_x;
    for (int i = 0; i < ncells; i++, x += t->cell_w) {
      const PtCell *cl = &cells[i];
      row_has_uo |= cl->underline != PT_UNDERLINE_NONE ||
          (cl->style & PT_CELL_STYLE_OVERLINE) != 0 || cl->has_link ||
          (i >= span_a && i <= span_b);
      row_has_strike |= (cl->style & PT_CELL_STYLE_STRIKE) != 0;
      text_blink |= (cl->style & PT_CELL_STYLE_BLINK) != 0;
      gboolean paint = TRUE;
      GdkRGBA color = { 0, 0, 0, 0 };
      if (cl->selected)
        color = sel_rgba;
      else if (cl->text[0] != '\0' && (cl->style & PT_CELL_STYLE_INVERSE))
        /* inverse paints the cell's own foreground behind the text */
        color = (GdkRGBA){ cl->fg.r / 255.0f, cl->fg.g / 255.0f,
                           cl->fg.b / 255.0f, 1 };
      else if (cl->has_bg)
        color = (GdkRGBA){ cl->bg.r / 255.0f, cl->bg.g / 255.0f,
                           cl->bg.b / 255.0f, 1 };
      else
        paint = FALSE;
      if (paint && bg_w > 0 && gdk_rgba_equal(&color, &bg_color)) {
        bg_w += t->cell_w;
        continue;
      }
      if (bg_w > 0)
        gtk_snapshot_append_color(snapshot, &bg_color,
            &GRAPHENE_RECT_INIT(bg_x, y, bg_w, t->cell_h));
      bg_w = 0;
      if (paint) { bg_x = x; bg_w = t->cell_w; bg_color = color; }
    }
    if (bg_w > 0)
      gtk_snapshot_append_color(snapshot, &bg_color,
          &GRAPHENE_RECT_INIT(bg_x, y, bg_w, t->cell_h));

    /* SGR underline and overline next, under the glyphs that are about to go
     * down — see the ---- decorations ---- comment for why this pass has to
     * come before the glyph loop rather than after it, the way it used to.
     * This is also where a link cell's underline is decided, so span_a/
     * span_b travel in here too. */
    if (row_has_uo)
      draw_row_underline_overline(t, snapshot, cells, ncells, y, bg_default,
                                  span_a, span_b);

    /* Glyphs third, over the row's backgrounds and its underline/overline. */
    x = pad_x;
    for (int i = 0; i < ncells; i++, x += t->cell_w) {
      const PtCell *cl = &cells[i];
      /* Nothing to draw: an empty cell, or the spacer half of a wide one. */
      if (cl->text[0] == '\0') {
        flush_run(snapshot, run_font, run, &run_color, run_x, y, t->baseline);
        continue;
      }
      gunichar cp = g_utf8_get_char(cl->text);
      gboolean single = *g_utf8_next_char(cl->text) == '\0';
      /* A space paints nothing, and most of a terminal screen is spaces —
       * shaping them was the single largest slice of the old frame cost. */
      if (single && cp == ' ') {
        flush_run(snapshot, run_font, run, &run_color, run_x, y, t->baseline);
        continue;
      }
      /* INVISIBLE hides the glyph outright: ghostty `continue`s past an
       * invisible cell before it ever reaches the underline/overline code
       * (generic.zig:2920-2930), so the decoration pass below never sees
       * this cell either — invisible hides the whole cell, not just the
       * text, and the comment there says the choice is deliberate, to match
       * xterm rather than Alacritty.
       *
       * BLINK has no ghostty analogue at all — ghostty parses SGR 5 into
       * style.flags.blink but its renderer never reads it (grep -rn
       * "flags.blink" src/renderer/ in the vendored tree is empty; ghostty
       * does not blink text). pt implements it anyway, going further than
       * ghostty on purpose, and reuses the cursor's own timer and phase
       * (sync_blink_timer, blink_visible) so the two blink together instead
       * of at odds — a page with a blinking cursor next to blinking text
       * that blinked out of sync would be worse than either alone. An
       * unfocused pane's blink_visible is pinned TRUE by blink_timer_stop,
       * so this never hides text in a pane that is not focused, matching
       * the cursor's own "unfocused never blinks" rule. */
      gboolean invisible = (cl->style & PT_CELL_STYLE_INVISIBLE) != 0;
      gboolean blink_off =
          (cl->style & PT_CELL_STYLE_BLINK) != 0 && !t->blink_visible;
      if (invisible || blink_off) {
        flush_run(snapshot, run_font, run, &run_color, run_x, y, t->baseline);
        continue;
      }
      /* Under inverse video the glyph takes the cell's background colour;
       * the cell's own fg went down behind it in the first pass. */
      PtColor fg = (cl->style & PT_CELL_STYLE_INVERSE)
                       ? (cl->has_bg ? cl->bg : bg_default)
                       : cl->fg;
      /* FAINT multiplies the glyph's alpha by ghostty's faint-opacity
       * default, 0.5 (config/Config.zig:3716, applied at generic.zig:2878
       * and carried into every foreground draw for the cell) — pt has no
       * setting for this either, so 0.5 is a literal matched to the
       * reference implementation, same as the decoration pass above. */
      float alpha = (cl->style & PT_CELL_STYLE_FAINT) ? 0.5f : 1.0f;

      /* Block elements are drawn from the cell metrics, never shaped: the
       * font's ink is narrower than the rounded cell width, which seams every
       * boundary between adjacent block cells. */
      PtBlockRect rects[4];
      int nrects = single ? pt_block_glyph_rects(cp, rects) : 0;
      if (nrects > 0) {
        flush_run(snapshot, run_font, run, &run_color, run_x, y, t->baseline);
        for (int r = 0; r < nrects; r++)
          gtk_snapshot_append_color(snapshot,
              &(GdkRGBA){fg.r / 255.0f, fg.g / 255.0f, fg.b / 255.0f,
                         rects[r].alpha * alpha},
              &GRAPHENE_RECT_INIT(x + rects[r].x * t->cell_w,
                                  y + rects[r].y * t->cell_h,
                                  rects[r].w * t->cell_w,
                                  rects[r].h * t->cell_h));
        continue;
      }

      const GlyphEntry *ge =
          glyph_lookup(t, cl->text, strlen(cl->text), cp, single,
                       (cl->style & PT_CELL_STYLE_BOLD) != 0,
                       (cl->style & PT_CELL_STYLE_ITALIC) != 0);
      if (ge == NULL) continue;

      GdkRGBA fg_rgba = { fg.r / 255.0f, fg.g / 255.0f, fg.b / 255.0f, alpha };
      if (run->num_glyphs > 0 &&
          (ge->font != run_font || !gdk_rgba_equal(&fg_rgba, &run_color)))
        flush_run(snapshot, run_font, run, &run_color, run_x, y, t->baseline);
      if (run->num_glyphs == 0) {
        run_font = ge->font;
        run_color = fg_rgba;
        run_x = x;
      }
      int at = run->num_glyphs;
      pango_glyph_string_set_size(run, at + ge->glyphs->num_glyphs);
      memcpy(run->glyphs + at, ge->glyphs->glyphs,
             sizeof(PangoGlyphInfo) * ge->glyphs->num_glyphs);
      for (int g = 0; g < ge->glyphs->num_glyphs; g++)
        run->log_clusters[at + g] = at + g;
    }
    flush_run(snapshot, run_font, run, &run_color, run_x, y, t->baseline);
    /* Strikethrough last, over the glyphs — see the ---- decorations ----
     * comment for why strikethrough stayed on this side while
     * underline/overline (and the link merge) moved to the other. */
    if (row_has_strike)
      draw_row_strike(t, snapshot, cells, ncells, y, bg_default);
    y += t->cell_h;
  }
  pt_term_core_rows_end(rows);
  pango_glyph_string_free(run);
  t->text_blink = text_blink;

  /* cursor: one core call carries position, style, blink, color and width. */
  PtCursorInfo ci;
  gboolean cursor_in_vp = pt_term_core_cursor_info(t->core, &ci);
  t->blink_wanted = ci.blinking;
  sync_blink_timer(t);         /* the app or the frame just walked may have
                                 * started or stopped wanting a timer */
  PtCursorShape shape = cursor_shape(t, cursor_in_vp, &ci);
  if (shape != PT_CURSOR_NONE) {
    /* A wide character owns two cells and the cursor has to cover both, or it
     * lands over half a glyph and looks like a rendering fault. The core has
     * already resolved both ways in (ghostty's order, renderer/generic.zig:
     * 3232): x is backed up off a spacer tail, and width says 2 on either
     * half — so this draws exactly what it is told. */
    float x = pad_x + ci.x * t->cell_w;
    float y_cur = pad_y + ci.y * t->cell_h;
    float w_cur = (float)(ci.width * t->cell_w);
    /* A filled block sits on top of its glyph, so it stays translucent enough
     * to read through. The thin shapes have nothing under them to preserve and
     * would read as a smudge at the same alpha, so they are drawn solid — the
     * same reason the hollow outline has always been drawn brighter. */
    GdkRGBA solid = { ci.color.r / 255.0f, ci.color.g / 255.0f,
                      ci.color.b / 255.0f, 1.0f };
    switch (shape) {
      case PT_CURSOR_BLOCK:
        gtk_snapshot_append_color(snapshot,
            &(GdkRGBA){ ci.color.r / 255.0f, ci.color.g / 255.0f,
                        ci.color.b / 255.0f, 0.55f },
            &GRAPHENE_RECT_INIT(x, y_cur, w_cur, t->cell_h));
        break;
      case PT_CURSOR_BAR:
        /* Half the rule's thickness hangs over the left cell edge, so the bar
         * sits between two characters rather than biased onto the one it is
         * in front of (font/sprite/draw/special.zig:325). */
        gtk_snapshot_append_color(snapshot, &solid,
            &GRAPHENE_RECT_INIT(x - 1, y_cur, 2, t->cell_h));
        break;
      case PT_CURSOR_UNDERLINE:
        gtk_snapshot_append_color(snapshot, &solid,
            &GRAPHENE_RECT_INIT(x, y_cur + t->cell_h - 2, w_cur, 2));
        break;
      case PT_CURSOR_BLOCK_HOLLOW: {
        /* The alpha is higher than the filled block's because a 1px outline
         * reads much fainter at 0.55f. */
        GdkRGBA out = { ci.color.r / 255.0f, ci.color.g / 255.0f,
                        ci.color.b / 255.0f, 0.8f };
        GskRoundedRect cr;
        gsk_rounded_rect_init_from_rect(&cr,
            &GRAPHENE_RECT_INIT(x, y_cur, w_cur, t->cell_h), 0);
        gtk_snapshot_append_border(snapshot, &cr,
            (float[4]){ 1, 1, 1, 1 },
            (GdkRGBA[4]){ out, out, out, out });
        break;
      }
      case PT_CURSOR_NONE: break;   /* unreachable; keeps the switch total */
    }
  }

  /* unfocused scrim, in place of the old focused-pane ring: instead of
   * outlining the pane that has focus, we wash every other pane toward the
   * background, so the focused one is simply the one that still looks vivid.
   * Drawn last over the content, which also dims the hollow cursor above. */
  if (!t->focused) {
    gtk_snapshot_append_color(snapshot,
        &(GdkRGBA){th_bg.r / 255.0f, th_bg.g / 255.0f, th_bg.b / 255.0f, 0.35f},
        &GRAPHENE_RECT_INIT(0, 0, w, h));
  }

  /* scrollback scrollbar. Drawn after the scrim rather than before it: the
   * pointer can scroll a pane that does not have focus, and washing the one
   * affordance that says where you are toward the background is the wrong way
   * round. */
  double alpha = bar_alpha(t);
  guint64 sb_total = 0, sb_offset = 0, sb_len = 0;
  if (alpha > 0.0 && bar_thumb(t, &sb_total, &sb_offset, &sb_len)) {
    float track_y = PT_BAR_MARGIN;
    float track_h = (float)h - 2 * PT_BAR_MARGIN;
    float thumb_h = track_h * (float)sb_len / (float)sb_total;
    if (thumb_h < PT_BAR_MIN_H) thumb_h = PT_BAR_MIN_H;
    if (thumb_h > track_h) thumb_h = track_h;
    /* offset runs 0..total-len, so the thumb runs the length of the track it
     * does not itself occupy. */
    float travel = (float)sb_offset / (float)(sb_total - sb_len);
    if (travel > 1.0f) travel = 1.0f;   /* never past the end of the track */
    GskRoundedRect rr;
    gsk_rounded_rect_init_from_rect(&rr,
        &GRAPHENE_RECT_INIT((float)w - PT_BAR_MARGIN - PT_BAR_W,
                            track_y + (track_h - thumb_h) * travel,
                            PT_BAR_W, thumb_h),
        PT_BAR_W / 2.0f);
    gtk_snapshot_push_rounded_clip(snapshot, &rr);
    gtk_snapshot_append_color(snapshot,
        &(GdkRGBA){ th_slider.r / 255.0f, th_slider.g / 255.0f,
                    th_slider.b / 255.0f, (float)(th_slider.a * alpha) },
        &rr.bounds);
    gtk_snapshot_pop(snapshot);
  }

  /* exited banner */
  if (t->exited) {
    char msg[96];
    g_snprintf(msg, sizeof(msg),
               "[process exited: %d]  Enter=restart  Ctrl+Shift+W=close",
               t->exit_status);
    gtk_snapshot_append_color(snapshot, &(GdkRGBA){0, 0, 0, 0.75f},
        &GRAPHENE_RECT_INIT(0, h - t->cell_h - 8, w, t->cell_h + 8));
    pango_layout_set_attributes(t->layout, NULL);
    pango_layout_set_text(t->layout, msg, -1);
    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot,
                           &GRAPHENE_POINT_INIT(pad_x, h - t->cell_h - 4));
    gtk_snapshot_append_layout(snapshot, t->layout,
                               &(GdkRGBA){0.9f, 0.76f, 0.48f, 1});
    gtk_snapshot_restore(snapshot);
  }

  if (pt_debug_enabled())
    g_debug("pt frame: %.2f ms (%dx%d cells)",
            (double)(g_get_monotonic_time() - frame_t0) / 1000.0,
            t->cell_w > 0 ? (w - 2 * pad_x) / t->cell_w : 0,
            t->cell_h > 0 ? (h - 2 * pad_y) / t->cell_h : 0);
}

/* ---- input ---- */
static void restart_shell(PtTerminal *t) {
  char *cwd = pt_terminal_current_cwd(t);
  if (t->core != NULL) pt_term_core_free(t->core);
  t->core = NULL;
  t->exited = FALSE;
  sync_blink_timer(t);
  g_free(t->start_cwd);
  t->start_cwd = cwd != NULL ? cwd : g_strdup(g_get_home_dir());
  link_cache_reset(t);       /* the new core's serial starts over */
  url_clear(t);              /* and its grid holds none of the old text */
  /* Same reason: the agent that owned the dead session is not running in the
   * fresh shell, so its title must not stay on the tab until something else
   * sets one. */
  g_clear_pointer(&t->last_title, g_free);
  ensure_core(t);
  gtk_widget_queue_allocate(GTK_WIDGET(t)); /* re-sizes the new core */
  gtk_widget_queue_draw(GTK_WIDGET(t));
}

static void on_focus_enter(GtkEventControllerFocus *ctl, gpointer user) {
  (void)ctl;
  PtTerminal *t = PT_TERMINAL(user);
  t->focused = TRUE;
  sync_blink_timer(t);         /* only the focused pane blinks */
  /* Deliberately synchronous, where ghostty defers to a glib idle
   * (apprt/gtk/class/surface.zig:2750): it does so to avoid re-entering
   * libghostty while the renderer lock is held, and pt has no such lock. */
  if (t->core != NULL) pt_term_core_focus_report(t->core, TRUE, FALSE);
  gtk_widget_add_css_class(GTK_WIDGET(t), "focused");
  gtk_widget_queue_draw(GTK_WIDGET(t));
}

static void on_focus_leave(GtkEventControllerFocus *ctl, gpointer user) {
  (void)ctl;
  PtTerminal *t = PT_TERMINAL(user);
  t->focused = FALSE;
  sync_blink_timer(t);         /* and the timer goes with the focus */
  /* Also fires when the pane is unparented on a tab or project switch, and
   * when the window itself goes inactive, both by way of GTK's own crossing
   * events — so a backgrounded pane counts as unfocused with no code here.
   *
   * The second of those is easy to doubt, because GtkWindow keeps its focus
   * widget across deactivation. It holds anyway: _gtk_window_set_is_active
   * synthesizes a focus-out crossing of type GTK_CROSSING_ACTIVE from the
   * focus widget (gtkwindow.c), and GtkEventControllerFocus handles ACTIVE
   * crossings exactly as it handles FOCUS ones (gtkeventcontrollerfocus.c),
   * so "leave" is emitted. Everything that reads t->focused depends on this —
   * mode 1004 focus reports, and the notification suppression in the core. */
  if (t->core != NULL) pt_term_core_focus_report(t->core, FALSE, FALSE);
  gtk_widget_remove_css_class(GTK_WIDGET(t), "focused");
  gtk_widget_queue_draw(GTK_WIDGET(t));
}

static gboolean on_key_pressed(GtkEventControllerKey *ctl, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user) {
  (void)ctl; (void)keycode;
  PtTerminal *t = PT_TERMINAL(user);
  if (t->exited) {
    if (keyval == GDK_KEY_Return) { restart_shell(t); return TRUE; }
    return FALSE;
  }
  if (t->core == NULL) return FALSE;

  GhosttyKey key = pt_keymap_from_keyval(keyval);
  GhosttyMods mods = pt_keymap_mods(state);
  guint32 unshifted = pt_keymap_unshifted_codepoint(keyval);

  char utf8[8];
  gsize utf8_len = 0;
  guint32 uc = gdk_keyval_to_unicode(keyval);
  if (uc != 0 && g_unichar_isprint(uc) &&
      (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) == 0)
    utf8_len = g_unichar_to_utf8((gunichar)uc, utf8);

  gboolean consumed =
      pt_term_core_send_key(t->core, key, GHOSTTY_KEY_ACTION_PRESS, mods,
                            unshifted, utf8, utf8_len);
  if (consumed) {
    /* any keypress that writes to the pty drops the selection, and snaps the
     * viewport back to the prompt: typing into scrollback you can't see is how
     * you run a command without noticing it ran. Both calls fire the core's
     * draw callback when they change anything, so nothing is queued here — a
     * keypress that moves nothing repaints nothing. */
    pt_term_core_selection_clear(t->core);
    pt_term_core_scroll_bottom(t->core);
  }
  return consumed;
}

/* ---- mouse ----
 *
 * An app that enables mouse tracking (Claude Code, vim, htop, lazygit) can own
 * the pointer: wheel, buttons and motion are encoded to the pty instead of
 * driving selection or the viewport. Two things have to agree before that
 * happens, exactly as in ghostty: the app must have asked for the mouse, and
 * `mouse-reporting` must be on. It ships on, so those apps behave the way they
 * do everywhere else, and holding shift takes the pointer back for the length
 * of one gesture, which is the escape hatch every other terminal offers. Set
 * it off to make a plain drag always select pt's own text instead.
 */
/* Rows per wheel notch, as before. Pixel-unit deltas (touchpads, and wheels
 * whose driver reports high-resolution scrolling) already carry the distance
 * the content should travel, so they convert by cell height and nothing else:
 * a 21px drag on a 21px cell is one row, which keeps the terminal moving at
 * the same rate as the rest of the desktop. */
#define PT_SCROLL_ROWS 3

static GdkModifierType controller_mods(GtkEventController *ctl) {
  GdkEvent *ev = gtk_event_controller_get_current_event(ctl);
  return ev != NULL ? gdk_event_get_modifier_state(ev) : 0;
}

static gboolean mouse_reporting(PtTerminal *t, GdkModifierType state) {
  return t->core != NULL && t->report_mouse && (state & GDK_SHIFT_MASK) == 0 &&
         pt_term_core_mouse_tracking(t->core);
}

/* Who owns the pointer *right now*. While a button is held the answer was
 * settled at press time and does not change: letting go of shift halfway
 * through a selection must not start feeding the app the rest of the drag,
 * and pressing it mid-drag must not strand the app with a button that never
 * comes back up. With no button down (plain hover under mode 1003) there is
 * no gesture to be consistent with, so the current modifiers decide. */
static gboolean pointer_reports(PtTerminal *t, GdkModifierType state) {
  return t->button_down ? t->reporting_drag : mouse_reporting(t, state);
}

/* Who owns the *wheel*, which is not the same question. `mouse-reporting` is a
 * statement about the press: turning it off means a plain drag selects text
 * inside a full-screen TUI without learning a modifier first. The wheel got
 * swept up in that, and an app tracking the mouse on the alt screen took the
 * wheel out of pt's hands (no scrollback to move) without being given it, so
 * scrolling did nothing at all. So the wheel goes to any app that tracks the
 * mouse whatever the key says, and shift takes it back, which is what xterm,
 * kitty and ghostty all do. Mid-drag it still follows whoever took the press,
 * for the same reason the pointer does. */
static gboolean wheel_reports(PtTerminal *t, GdkModifierType state) {
  if (t->button_down) return t->reporting_drag;
  return t->core != NULL && (state & GDK_SHIFT_MASK) == 0 &&
         pt_term_core_mouse_tracking(t->core);
}

static GhosttyMouseButton ghostty_button(guint gdk_button) {
  switch (gdk_button) {
  case GDK_BUTTON_PRIMARY:   return GHOSTTY_MOUSE_BUTTON_LEFT;
  case GDK_BUTTON_MIDDLE:    return GHOSTTY_MOUSE_BUTTON_MIDDLE;
  case GDK_BUTTON_SECONDARY: return GHOSTTY_MOUSE_BUTTON_RIGHT;
  case 8:                    return GHOSTTY_MOUSE_BUTTON_FOUR;
  case 9:                    return GHOSTTY_MOUSE_BUTTON_FIVE;
  default:                   return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
  }
}

/* The hand cursor over a link, which is how a user finds out the underline
 * means something. Latched so a pointer sitting still on a link is not setting
 * the same cursor once per motion event. */
static void set_link_cursor(PtTerminal *t, gboolean on) {
  if (t->link_cursor == on) return;
  t->link_cursor = on;
  gtk_widget_set_cursor_from_name(GTK_WIDGET(t), on ? "pointer" : NULL);
}

/* Shift decides who owns the pointer, so the cursor needs it as it is *now* —
 * not as of the last event this pane happened to see. It cannot be read off
 * the event that prompted the question: keys only reach the focused pane, and
 * a crossing has no event to read at all (GTK leaves the controller with no
 * current event during `enter` — measured, not assumed). The seat's keyboard
 * is the one source that is always current, whichever pane is asking. */
static GdkModifierType live_mods(void) {
  GdkDisplay *display = gdk_display_get_default();
  GdkSeat *seat = display != NULL ? gdk_display_get_default_seat(display) : NULL;
  GdkDevice *kbd = seat != NULL ? gdk_seat_get_keyboard(seat) : NULL;
  return kbd != NULL ? gdk_device_get_modifier_state(kbd) : 0;
}

/* The cached answer no longer holds: something other than the pointer's cell
 * or the grid's content changed it (modifiers, a mouse-reporting flip, a
 * button gesture settling ownership), so the next ask has to do the work. */
static void link_cache_reset(PtTerminal *t) {
  t->link_row = -2;
}

/* Drops the highlighted bare URL and repaints if one was up, since the
 * underline is drawn from it. */
static void url_clear(PtTerminal *t) {
  if (t->url_uri == NULL) return;
  g_clear_pointer(&t->url_uri, g_free);
  gtk_widget_queue_draw(GTK_WIDGET(t));
}

/* The bare URL covering cell (row, col), stored with the cells it covers.
 * Nothing here is OSC 8: this is an address a program merely printed, found by
 * matching the logical line the way ghostty does (Surface.zig linkAtPin), and
 * it is only looked for while the link modifier is down — ghostty's default
 * link carries `hover_mods = ctrl`, and highlighting every URL on screen all
 * the time would underline half of a build log. */
static void url_update(PtTerminal *t, int row, int col) {
  PtLine line;
  if (!pt_term_core_line_at(t->core, row, &line)) { url_clear(t); return; }

  /* Cell -> byte: the first byte the hovered cell drew. A cell that drew
   * nothing (the spacer half of a wide character) is not on any address. */
  gsize at = 0;
  gboolean have = FALSE;
  for (gsize i = 0; i < line.len; i++)
    if (line.at[i].row == row && line.at[i].col == col) { at = i; have = TRUE; break; }

  PtLinkSpan span;
  if (!have || !pt_link_find_at(line.text, at, &span) ||
      span.end <= span.start || span.end > line.len) {
    pt_term_core_line_clear(&line);
    url_clear(t);
    return;
  }

  char *uri = g_strndup(line.text + span.start, span.end - span.start);
  /* The pattern only offers schemes pt opens, so this is belt and braces —
   * but the text came off the pty, and nothing unopenable may be underlined. */
  if (!pt_term_core_hyperlink_is_safe(uri)) {
    g_free(uri);
    pt_term_core_line_clear(&line);
    url_clear(t);
    return;
  }

  PtCellPos a = line.at[span.start], b = line.at[span.end - 1];
  gboolean same = t->url_uri != NULL && g_strcmp0(t->url_uri, uri) == 0 &&
                  t->url_r0 == a.row && t->url_c0 == a.col &&
                  t->url_r1 == b.row && t->url_c1 == b.col;
  g_free(t->url_uri);
  t->url_uri = uri;
  t->url_r0 = a.row; t->url_c0 = a.col;
  t->url_r1 = b.row; t->url_c1 = b.col;
  pt_term_core_line_clear(&line);
  /* Only a *different* answer changes the frame; hovering along one URL
   * must not queue a repaint per cell. */
  if (!same) gtk_widget_queue_draw(GTK_WIDGET(t));
}

/* Asks again what is under the pointer where it already is. The pointer is not
 * the only thing that moves: output scrolls the grid out from under it and a
 * resize reflows it, either of which can take the link away or bring one in
 * without a single motion event. So the answer is re-derived on redraw too,
 * from the last position seen — and, because most motion stays inside one cell
 * and most redraws change nothing, cached by (cell, content serial): while
 * both hold, the seat walk and the grid lookup are skipped entirely. */
static void update_link_cursor(PtTerminal *t) {
  if (!t->pointer_in || t->core == NULL) return;
  /* Pixel -> cell, mirroring the core's own mapping. Everywhere outside the
   * grid shares one bucket (-1): every outside answer is "no link". */
  int col = -1, row = -1;
  double cx = (t->mouse_x - pad_x) / (double)t->cell_w;
  double cy = (t->mouse_y - pad_y) / (double)t->cell_h;
  if (cx >= 0 && cy >= 0) { col = (int)cx; row = (int)cy; }
  guint serial = pt_term_core_content_serial(t->core);
  if (row == t->link_row && col == t->link_col && serial == t->link_serial)
    return;
  t->link_row = row;
  t->link_col = col;
  t->link_serial = serial;
  GdkModifierType mods = live_mods();
  /* The app owns the pointer, links included: ⌃click goes to it, so the
   * cursor must not promise otherwise. */
  if (pointer_reports(t, mods)) {
    url_clear(t);
    set_link_cursor(t, FALSE);
    return;
  }
  /* OSC 8 first, exactly as ghostty orders it: a program that declared a link
   * on this cell has said more about it than any pattern can infer. */
  char *uri = row >= 0 ? pt_term_core_link_at_cell(t->core, row, col) : NULL;
  if (uri != NULL) {
    url_clear(t);
    set_link_cursor(t, TRUE);
    g_free(uri);
    return;
  }
  if (row >= 0 && (mods & GDK_CONTROL_MASK) != 0)
    url_update(t, row, col);
  else
    url_clear(t);
  set_link_cursor(t, t->url_uri != NULL);
}

static void on_motion(GtkEventControllerMotion *ctl, double x, double y,
                      gpointer user) {
  PtTerminal *t = PT_TERMINAL(user);
  t->mouse_x = x;
  t->mouse_y = y;
  t->pointer_in = TRUE;
  GdkModifierType state = controller_mods(GTK_EVENT_CONTROLLER(ctl));
  update_link_cursor(t);
  if (!pointer_reports(t, state)) return;
  /* The core drops motion the app didn't ask for (press-only modes) and
   * repeats within one cell, so this stays cheap on every pointer move. */
  pt_term_core_mouse_report(t->core, GHOSTTY_MOUSE_ACTION_MOTION,
                            GHOSTTY_MOUSE_BUTTON_UNKNOWN, pt_keymap_mods(state),
                            x, y);
}

/* A pane can arrive under a pointer that is not moving — a new window maps, a
 * split rearranges — and then no motion is coming to notice the link below it.
 * Deliberately reports nothing to the app: pt reports motion, and the pointer
 * has not moved. */
static void on_motion_enter(GtkEventControllerMotion *ctl, double x, double y,
                            gpointer user) {
  (void)ctl;
  PtTerminal *t = PT_TERMINAL(user);
  t->mouse_x = x;
  t->mouse_y = y;
  t->pointer_in = TRUE;
  update_link_cursor(t);
}

static void on_motion_leave(GtkEventControllerMotion *ctl, gpointer user) {
  (void)ctl;
  PtTerminal *t = PT_TERMINAL(user);
  t->pointer_in = FALSE;
  set_link_cursor(t, FALSE);
  /* A bare URL is highlighted because the pointer is on it; it is not. */
  url_clear(t);
  /* The cursor was just forced off; re-entering the same cell has to be able
   * to bring it back even if nothing else moved. */
  link_cache_reset(t);
}

/* Shift is the override that takes the pointer back from an app tracking it,
 * so pressing or releasing it changes the answer for a pointer that never
 * moved. Only a prod to go and look — the state itself comes from the seat.
 * Keys reach the focused pane alone, so shift changing while the pointer sits
 * over some *other* pane still goes unnoticed until anything at all happens
 * there; cosmetic, and the click itself has never used this path. */
static gboolean on_key_modifiers(GtkEventControllerKey *ctl,
                                 GdkModifierType state, gpointer user) {
  (void)ctl; (void)state;
  /* Modifiers change the answer without moving the pointer or the grid, so
   * the cache would swallow the update. */
  link_cache_reset(PT_TERMINAL(user));
  update_link_cursor(PT_TERMINAL(user));
  return GDK_EVENT_PROPAGATE;
}

static void on_uri_launched(GObject *src, GAsyncResult *res, gpointer user) {
  (void)user;
  GError *err = NULL;
  if (!gtk_uri_launcher_launch_finish(GTK_URI_LAUNCHER(src), res, &err))
    g_warning("pt: could not open %s: %s",
              gtk_uri_launcher_get_uri(GTK_URI_LAUNCHER(src)),
              err != NULL ? err->message : "?");
  g_clear_error(&err);
}

/* Whether cell (row, col) is inside the highlighted bare URL. The match runs
 * along a logical line, so on screen it is a range in reading order: the tail
 * of its first row, whole rows after that, and the head of its last. */
static gboolean url_covers(PtTerminal *t, int row, int col) {
  if (t->url_uri == NULL) return FALSE;
  if (row < t->url_r0 || row > t->url_r1) return FALSE;
  if (row == t->url_r0 && col < t->url_c0) return FALSE;
  if (row == t->url_r1 && col > t->url_c1) return FALSE;
  return TRUE;
}

/* TRUE when there was a link under the pointer and it was handed to the
 * desktop. Both kinds are only ever URIs with a scheme pt is willing to open —
 * the core filters OSC 8, and the hover pass filters what it matched — so this
 * is the whole of the check. */
static gboolean open_link_at(PtTerminal *t, double x, double y) {
  char *uri = pt_term_core_hyperlink_at(t->core, x, y);
  if (uri == NULL) {
    /* The bare URL the hover already resolved for this cell. Re-checked
     * against the click's own position rather than trusted: the pointer can
     * reach a cell the hover pass never answered for. */
    int col = (int)((x - pad_x) / (double)t->cell_w);
    int row = (int)((y - pad_y) / (double)t->cell_h);
    if (x < pad_x || y < pad_y || !url_covers(t, row, col)) return FALSE;
    uri = g_strdup(t->url_uri);
  }
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(t));
  GtkUriLauncher *launcher = gtk_uri_launcher_new(uri);
  gtk_uri_launcher_launch(launcher,
                          GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL,
                          NULL, on_uri_launched, NULL);
  g_object_unref(launcher);
  g_free(uri);
  return TRUE;
}

static gboolean on_scroll(GtkEventControllerScroll *ctl, double dx, double dy,
                          gpointer user) {
  (void)dx;
  PtTerminal *t = PT_TERMINAL(user);
  if (t->core == NULL) return FALSE;

  /* Wheels report in notches, touchpads in pixels: normalize both to rows and
   * keep the remainder, so slow trackpad scrolling accumulates into a row
   * instead of being rounded away. */
  gboolean pixels =
      gtk_event_controller_scroll_get_unit(ctl) == GDK_SCROLL_UNIT_SURFACE;
  /* Two currencies, tracked side by side so switching between a tracking app
   * and the local viewport mid-gesture doesn't mix them up. Scrolling pt's own
   * grid is measured in rows; an app that owns the wheel is fed *notches*,
   * because that is what a wheel physically sends and what the app is tuned
   * for — one report per row would triple a wheel's traffic and bury a TUI
   * under a touchpad's event rate, which surfaces as the view lagging behind
   * the pointer. */
  double rows_f = pixels ? dy / (double)t->cell_h : dy * PT_SCROLL_ROWS;
  /* A wheel notch is one report; pixel travel reports every row, which keeps
   * the app responding to short gestures. Coarser thresholds delay the first
   * report until the pointer has travelled far enough, which reads as lag. */
  double notches_f = pixels ? dy / (double)t->cell_h : dy;

  double pend_rows = t->scroll_pending + rows_f;
  int rows = (int)trunc(pend_rows);
  t->scroll_pending = pend_rows - rows;

  double pend_notches = t->report_pending + notches_f;
  int notches = (int)trunc(pend_notches);
  t->report_pending = pend_notches - notches;

  if (pt_debug_enabled())
    g_debug("pt scroll: dy=%.3f unit=%s cell_h=%d -> rows=%d notches=%d",
            dy, pixels ? "pixels" : "notches", t->cell_h, rows, notches);

  GdkModifierType state = controller_mods(GTK_EVENT_CONTROLLER(ctl));

  /* Not pointer_reports(): the wheel is not gated on `mouse-reporting`. It is
   * still latched mid-drag though, so scrolling to extend a selection past the
   * viewport moves pt's scrollback instead of landing in the app. */
  if (wheel_reports(t, state)) {
    if (notches == 0) return TRUE;
    /* One button-4/5 press per notch, reported at the pointer: GTK scroll
     * events carry no coordinates of their own. The core encodes the whole
     * burst into a single write. Nothing local changed — selection_clear
     * fires the draw callback itself, and whatever the app does with the
     * reports comes back through the pty read path. */
    pt_term_core_selection_clear(t->core);
    GhosttyMouseButton btn = notches < 0 ? GHOSTTY_MOUSE_BUTTON_FOUR
                                         : GHOSTTY_MOUSE_BUTTON_FIVE;
    pt_term_core_wheel_report(t->core, btn, pt_keymap_mods(state),
                              t->mouse_x, t->mouse_y, ABS(notches));
    return TRUE;
  }

  if (rows == 0) return TRUE;

  /* Alt screen with alternate scroll (mode 1007, on by default) and no mouse
   * reporting: the wheel becomes cursor keys, which is what makes less, man
   * and git log scroll under a plain pager. */
  if (pt_term_core_alt_screen(t->core) && !pt_term_core_mouse_tracking(t->core) &&
      pt_term_core_alt_scroll(t->core)) {
    /* The arrows only matter once the app answers them, which comes back
     * through the read path; the selection clear queues its own repaint. */
    pt_term_core_selection_clear(t->core);
    pt_term_core_send_arrows(t->core, rows < 0, ABS(rows));
    return TRUE;
  }

  pt_term_core_scroll_delta(t->core, rows);
  bar_reveal(t);            /* the one place the viewport moves by hand */
  return TRUE;
}

static void on_click_pressed(GtkGestureClick *g, int n, double x, double y,
                             gpointer user) {
  (void)n;
  PtTerminal *t = PT_TERMINAL(user);
  gtk_widget_grab_focus(GTK_WIDGET(t));
  if (t->core == NULL) return;
  t->mouse_x = x;
  t->mouse_y = y;

  guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
  GdkModifierType state = controller_mods(GTK_EVENT_CONTROLLER(g));
  t->reporting_drag = mouse_reporting(t, state);
  t->button_down = TRUE;
  link_cache_reset(t);   /* pointer ownership was just settled for the drag */
  if (pt_debug_enabled())
    g_debug("pt press: btn=%u n=%d mods=0x%x tracking=%d report=%d -> %s",
            button, n, (unsigned)state, pt_term_core_mouse_tracking(t->core),
            t->report_mouse, t->reporting_drag ? "app" : "selection");
  if (t->reporting_drag) {
    /* The clear repaints by itself; the press only matters once the app
     * reacts, which arrives through the read path. */
    pt_term_core_selection_clear(t->core);
    pt_term_core_mouse_report(t->core, GHOSTTY_MOUSE_ACTION_PRESS,
                              ghostty_button(button), pt_keymap_mods(state),
                              x, y);
    return;
  }
  /* Locally only the primary button means anything: middle and right exist
   * for the app's sake, and dropping them here keeps them from clearing a
   * selection the user just made. */
  if (button != GDK_BUTTON_PRIMARY) return;

  /* ⌃click opens the link under the pointer. Ahead of the selection press so
   * the click that opened something does not also start a drag, and behind the
   * reporting branch above so an app that took the mouse still gets it. */
  if ((state & GDK_CONTROL_MASK) != 0 && open_link_at(t, x, y)) return;

  /* controller event time is in milliseconds; the core wants nanoseconds.
   * Selection changes (a dropped one, a double-click's word) repaint through
   * the core's draw callback; a plain first press changes nothing visible. */
  guint32 ms =
      gtk_event_controller_get_current_event_time(GTK_EVENT_CONTROLLER(g));
  pt_term_core_selection_press(t->core, x, y, (guint64)ms * 1000000ULL);
}

static void on_click_released(GtkGestureClick *g, int n, double x, double y,
                              gpointer user) {
  (void)n;
  PtTerminal *t = PT_TERMINAL(user);
  if (t->core == NULL) return;
  t->mouse_x = x;
  t->mouse_y = y;

  guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
  t->button_down = FALSE;   /* the gesture is over; ownership is open again */
  link_cache_reset(t);      /* and the link answer may change with it */
  /* Release follows whichever owner took the press, so an app that stops
   * tracking mid-drag still sees the button go up. Nothing local changed:
   * the app's reaction comes back through the read path. */
  if (t->reporting_drag) {
    GdkModifierType state = controller_mods(GTK_EVENT_CONTROLLER(g));
    pt_term_core_mouse_report(t->core, GHOSTTY_MOUSE_ACTION_RELEASE,
                              ghostty_button(button), pt_keymap_mods(state),
                              x, y);
    t->reporting_drag = FALSE;
    return;
  }
  if (button != GDK_BUTTON_PRIMARY) return;
  /* The final selection install repaints through the core's draw callback. */
  pt_term_core_selection_release(t->core, x, y);
}

/* The endings "released" never sees. A held press the app owns has a bit set
 * in the core's buttons_down, and the core substitutes a held button into
 * unnamed motion (pt_term_core_mouse_report) — so a press bit nobody clears
 * turns every later hover into a phantom SGR drag under mode 1002. pt has
 * shipped that leak before, through the wheel (see the buttons_down note in
 * pt_term_core_wheel_report). The app saw the press, so it is owed the
 * release; the modifiers are read off the seat because these paths carry no
 * event to read them from. */
static void mouse_gesture_cancel(PtTerminal *t) {
  t->button_down = FALSE;   /* ownership is open again */
  link_cache_reset(t);
  if (t->core != NULL && t->reporting_drag)
    pt_term_core_mouse_cancel(t->core, pt_keymap_mods(live_mods()),
                              t->mouse_x, t->mouse_y);
  t->reporting_drag = FALSE;
}

/* GTK cancels the click gesture instead of releasing it when something takes
 * the sequence away mid-press: a grab (a popup, a context menu, a drag
 * elsewhere in the window) or the widget's own teardown. Ghostty's GTK apprt
 * wires only press and release here; pt cannot afford to, for the reason
 * above. */
static void on_click_cancel(GtkGesture *g, GdkEventSequence *seq,
                            gpointer user) {
  (void)g; (void)seq;
  mouse_gesture_cancel(PT_TERMINAL(user));
}

static void on_drag_update(GtkGestureDrag *g, double ox, double oy,
                           gpointer user) {
  PtTerminal *t = PT_TERMINAL(user);
  if (t->core == NULL) return;
  /* Drags the app owns are reported from the motion controller, which fires
   * with the button held too; building a selection here as well would paint
   * one over the app's own. */
  if (t->reporting_drag) return;
  double sx = 0, sy = 0;
  gtk_gesture_drag_get_start_point(g, &sx, &sy);
  /* Each install along the drag repaints through the core's draw callback. */
  pt_term_core_selection_drag(t->core, sx + ox, sy + oy);
}

/* Clipboard text held across the confirmation dialog. */
typedef struct {
  PtTerminal *term;   /* owned ref */
  char *text;         /* owned */
} PtPasteCtx;

/* Runs when the dialog is finalized, so it covers every way one can go away:
 * the Paste response, the Cancel response, and dismissal (Escape, or clicking
 * outside), which AdwAlertDialog reports as the close response. */
static void paste_ctx_free(gpointer data, GClosure *closure) {
  (void)closure;
  PtPasteCtx *p = data;
  p->term->paste_pending = FALSE;
  g_object_unref(p->term);
  g_free(p->text);
  g_free(p);
}

static void on_paste_confirm_response(AdwAlertDialog *dlg, const char *response,
                                      gpointer user) {
  (void)dlg;
  PtPasteCtx *p = user;
  if (g_strcmp0(response, "paste") == 0 && p->term->core != NULL)
    pt_term_core_paste(p->term->core, p->text, -1);
  /* Cancelled or pasted, the keyboard belongs back in the pane. */
  if (gtk_widget_get_root(GTK_WIDGET(p->term)) != NULL)
    gtk_widget_grab_focus(GTK_WIDGET(p->term));
}

/* CR, LF and CRLF all break a line once. A break at the very end closes the
 * last line rather than opening another, so "a\nb\n" is two lines, not three. */
static guint paste_line_count(const char *text) {
  guint lines = 1;
  for (const char *p = text; *p != '\0'; p++) {
    if (*p != '\n' && *p != '\r') continue;
    if (*p == '\r' && p[1] == '\n') p++;
    if (p[1] == '\0') break;
    lines++;
  }
  return lines;
}

/* Text with a line break (or its own end-of-paste sequence) runs the moment it
 * lands in a shell, so it gets a look first. Takes ownership of `text`, of the
 * reference on `t`, and of `t`'s paste_pending reservation. */
static void present_paste_confirm(PtTerminal *t, char *text) {
  gboolean multiline = strpbrk(text, "\r\n") != NULL;
  guint lines = paste_line_count(text);
  char *heading = g_strdup_printf(lines == 1 ? "Paste %u line?"
                                             : "Paste %u lines?", lines);
  AdwDialog *dlg = adw_alert_dialog_new(
      heading,
      multiline ? "The shell runs each line as soon as it is pasted."
                : "This text ends a bracketed paste partway through, which "
                  "lets the rest of it run as a command.");
  g_free(heading);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dlg),
                                 "cancel", "Cancel", "paste", "Paste", NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "paste",
                                           ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dlg), "cancel");
  PtPasteCtx *p = g_new0(PtPasteCtx, 1);
  p->term = t;
  p->text = text;
  g_signal_connect_data(dlg, "response", G_CALLBACK(on_paste_confirm_response),
                        p, paste_ctx_free, 0);
  adw_dialog_present(dlg, GTK_WIDGET(t));
}

static void on_paste_text(GObject *src, GAsyncResult *res, gpointer user) {
  PtTerminal *t = PT_TERMINAL(user);
  char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
  /* Exactly one of the branches below may hand the reservation on, and the
   * tail releases it otherwise: leaking it would leave the pane unable to
   * paste for the rest of its life. */
  gboolean handed_on = FALSE;
  if (text != NULL && t->core != NULL) {
    /* The core sanitizes either way; the dialog is about what the text will do
     * once it gets there, not about what bytes reach the pty. */
    if (pt_term_core_paste_is_safe(text, -1)) {
      pt_term_core_paste(t->core, text, -1);
    } else if (gtk_widget_get_root(GTK_WIDGET(t)) != NULL) {
      present_paste_confirm(t, text);   /* takes text, the ref and the slot */
      handed_on = TRUE;
    }
    /* Unsafe with no window to ask in (the pane was unparented while the read
     * was in flight): dropped. Never pasted without asking. */
  }
  if (handed_on) return;
  g_free(text);
  t->paste_pending = FALSE;
  g_object_unref(t);
}

void pt_terminal_paste(PtTerminal *t) {
  if (t->paste_pending) return;   /* an earlier ⌃⇧V is still working */
  t->paste_pending = TRUE;
  GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(t));
  gdk_clipboard_read_text_async(cb, NULL, on_paste_text, g_object_ref(t));
}

void pt_terminal_copy(PtTerminal *t) {
  if (t->core == NULL) return;
  char *text = pt_term_core_selection_text(t->core);   /* g_strndup'd */
  if (text == NULL) return;
  gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(t)), text);
  g_free(text);
}

char *pt_terminal_current_cwd(PtTerminal *t) {
  if (t->core == NULL) return g_strdup(t->start_cwd);
  char proc[64];
  g_snprintf(proc, sizeof(proc), "/proc/%d/cwd",
             (int)pt_term_core_shell_pid(t->core));
  char *cwd = g_file_read_link(proc, NULL);
  return cwd != NULL ? cwd : g_strdup(t->start_cwd);
}

PtTermCore *pt_terminal_core(PtTerminal *t) { return t->core; }

guint64 pt_terminal_id(PtTerminal *t) { return t->id; }

const char *pt_terminal_last_command(PtTerminal *t) { return t->last_command; }

const char *pt_terminal_last_title(PtTerminal *t) { return t->last_title; }

/* Delegates rather than copies: the core derives the name from its own spawn
 * (see pt_term_core_shell_name), so a restart's fresh core brings a fresh name
 * with it and a failed respawn answers NULL instead of a stale one. */
const char *pt_terminal_shell_name(PtTerminal *t) {
  return t->core != NULL ? pt_term_core_shell_name(t->core) : NULL;
}

gboolean pt_terminal_running(PtTerminal *t) {
  return t->core != NULL && pt_term_core_running(t->core);
}

int pt_terminal_last_exit(PtTerminal *t) {
  return t->core != NULL ? pt_term_core_last_exit(t->core) : -1;
}

/* ---- theme ---- */
void pt_terminal_set_theme(const PtResolvedTheme *rt) {
  th_bg  = rt->term.background;
  th_fg  = rt->term.foreground;
  th_cursor = rt->term.cursor;
  th_sel = rt->term.selection_bg;
  th_ring = rt->tokens[PT_TOK_FOCUS_RING];
  th_slider = rt->tokens[PT_TOK_SLIDER];
  th_dark = rt->dark;
  for (int i = 0; i < 16; i++) {
    th_pal[i] = rt->term.palette[i];
    th_pal_set[i] = rt->term.palette_set[i];
  }
  for (GSList *l = live_terminals; l != NULL; l = l->next) {
    PtTerminal *t = l->data;
    if (t->core != NULL) {
      apply_palette(t->core);
      /* An app on mode 2031 hears about a light/dark flip here. This runs on
       * every settings-dialog preview step too, which is why the core drops
       * the ones that change nothing rather than this call site guarding. */
      pt_term_core_set_color_scheme(t->core, th_dark);
    }
    gtk_widget_queue_draw(GTK_WIDGET(t));
  }
}

/* ---- global font zoom ---- */
int pt_terminal_font_size(void) { return font_size_pts; }

void pt_terminal_set_font_size(int pts) { pt_terminal_set_font(NULL, pts); }

void pt_terminal_set_font(const char *family, int pts) {
  pts = CLAMP(pts, PT_FONT_SIZE_MIN, PT_FONT_SIZE_MAX);
  gboolean family_changed =
      family != NULL &&
      g_strcmp0(family, font_family != NULL ? font_family : "") != 0;
  if (!family_changed && pts == font_size_pts) return;
  if (family_changed) {
    g_free(font_family);
    font_family = g_strdup(family);
  }
  font_size_pts = pts;
  for (GSList *l = live_terminals; l != NULL; l = l->next) {
    PtTerminal *t = l->data;
    if (family_changed) {
      pango_font_description_free(t->font_desc);
      char *spec = g_strdup_printf("%s, monospace", font_family);
      t->font_desc = pango_font_description_from_string(spec);
      g_free(spec);
    }
    pango_font_description_set_size(t->font_desc, pts * PANGO_SCALE);
    pango_layout_set_font_description(t->layout, t->font_desc);
    measure_font(t);
    /* size_allocate re-derives cols/rows from the new cell metrics and
     * resizes the PTY + vt (reflow). */
    gtk_widget_queue_resize(GTK_WIDGET(t));
    gtk_widget_queue_draw(GTK_WIDGET(t));
  }
}

/* ---- grid inset ---- */
void pt_terminal_set_padding(int x, int y) {
  x = CLAMP(x, PT_CONFIG_WINDOW_PADDING_MIN, PT_CONFIG_WINDOW_PADDING_MAX);
  y = CLAMP(y, PT_CONFIG_WINDOW_PADDING_MIN, PT_CONFIG_WINDOW_PADDING_MAX);
  if (x == pad_x && y == pad_y) return;
  pad_x = x;
  pad_y = y;
  for (GSList *l = live_terminals; l != NULL; l = l->next) {
    PtTerminal *t = l->data;
    /* The core maps pixels to cells for selections, links and mouse reports,
     * so it has to learn the new inset in the same pass that redraws with
     * it. */
    if (t->core != NULL) pt_term_core_set_padding(t->core, pad_x, pad_y);
    /* size_allocate re-derives cols/rows from the new inset and resizes the
     * PTY + vt (reflow), exactly as a font change does. */
    gtk_widget_queue_resize(GTK_WIDGET(t));
    gtk_widget_queue_draw(GTK_WIDGET(t));
  }
}

void pt_terminal_set_scrollback_limit(gsize bytes) {
  scrollback_limit = bytes;
}

void pt_terminal_set_pane_mouse_reporting(PtTerminal *t, gboolean on) {
  t->report_mouse = on;
  link_cache_reset(t);       /* who owns the pointer just changed under it */
  update_link_cursor(t);
}

void pt_terminal_set_pane_osc52(PtTerminal *t, PtOsc52Mode mode) {
  t->osc52 = mode;
  /* Turning it off has to reach the core too, or a pane keeps decoding
   * clipboards nobody will ever be shown. */
  if (t->core != NULL) pt_term_core_set_osc52(t->core, mode);
}

void pt_terminal_set_mouse_reporting(gboolean on) {
  /* The file is the source of truth: a pane the user toggled by hand goes
   * back in step the next time the config is applied, same as the theme. */
  for (GSList *l = live_terminals; l != NULL; l = l->next)
    pt_terminal_set_pane_mouse_reporting(l->data, on);
}

void pt_terminal_set_osc52(PtOsc52Mode mode) {
  for (GSList *l = live_terminals; l != NULL; l = l->next)
    pt_terminal_set_pane_osc52(l->data, mode);
}

void pt_terminal_reset(PtTerminal *t) {
  if (t->core == NULL) return;      /* nothing has been spawned in this pane */
  /* The core drops its half of the gesture; these are the widget's half, and
   * they decide who owns the pointer for the rest of a drag. Cleared before the
   * reset so the redraw it fires already sees the settled state. */
  t->reporting_drag = FALSE;
  t->button_down = FALSE;
  pt_term_core_reset(t->core);
}

gboolean pt_terminal_mouse_reporting(PtTerminal *t) { return t->report_mouse; }

gboolean pt_terminal_toggle_mouse_reporting(PtTerminal *t) {
  t->report_mouse = !t->report_mouse;
  /* Handing the pointer to the app mid-selection would leave a highlight
   * nothing can clear. */
  if (t->report_mouse && t->core != NULL) pt_term_core_selection_clear(t->core);
  link_cache_reset(t);       /* who owns the pointer just changed under it */
  update_link_cursor(t);
  gtk_widget_queue_draw(GTK_WIDGET(t));
  return t->report_mouse;
}

/* ---- boilerplate ---- */
/* A pane can leave the screen with a button still held (a tab switch over a
 * held press); no release or gesture cancel can reach it once unmapped, so the
 * press would strand exactly like a cancelled sequence. Same treatment. */
static void pt_terminal_unmap(GtkWidget *widget) {
  mouse_gesture_cancel(PT_TERMINAL(widget));
  GTK_WIDGET_CLASS(pt_terminal_parent_class)->unmap(widget);
}

static void pt_terminal_dispose(GObject *obj) {
  PtTerminal *t = PT_TERMINAL(obj);
  live_terminals = g_slist_remove(live_terminals, t);
  if (t->bar_tick != 0) {
    gtk_widget_remove_tick_callback(GTK_WIDGET(t), t->bar_tick);
    t->bar_tick = 0;
  }
  g_clear_handle_id(&t->bar_hold, g_source_remove);
  blink_timer_stop(t);         /* the timer holds a pointer to this widget */
  g_clear_pointer(&t->core, pt_term_core_free);
  g_clear_object(&t->layout);
  g_clear_pointer(&t->font_desc, pango_font_description_free);
  g_clear_pointer(&t->start_cwd, g_free);
  g_clear_pointer(&t->env, g_strfreev);
  g_clear_pointer(&t->startup_input, g_free);  /* unconsumed when the pane never spawned */
  g_clear_pointer(&t->last_command, g_free);
  g_clear_pointer(&t->last_title, g_free);
  g_clear_pointer(&t->url_uri, g_free);
  G_OBJECT_CLASS(pt_terminal_parent_class)->dispose(obj);
}

static void pt_terminal_class_init(PtTerminalClass *klass) {
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  oc->dispose = pt_terminal_dispose;
  wc->snapshot = pt_terminal_snapshot;
  wc->size_allocate = pt_terminal_size_allocate;
  wc->unmap = pt_terminal_unmap;
  gtk_widget_class_set_css_name(wc, "pt-terminal");
  signals[SIG_EXITED] = g_signal_new("exited", PT_TYPE_TERMINAL,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
  signals[SIG_TITLE_CHANGED] = g_signal_new("title-changed", PT_TYPE_TERMINAL,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_COMMAND_CHANGED] = g_signal_new("command-changed", PT_TYPE_TERMINAL,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_NOTIFICATION] = g_signal_new("notification", PT_TYPE_TERMINAL,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2,
      G_TYPE_STRING, G_TYPE_STRING);
}

static void pt_terminal_init(PtTerminal *t) {
  t->id = next_terminal_id++;
  gtk_widget_set_focusable(GTK_WIDGET(t), TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(t), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(t), TRUE);
  char *spec = g_strdup_printf("%s, monospace",
      font_family != NULL ? font_family : PT_FONT_FAMILY_DEFAULT);
  t->font_desc = pango_font_description_from_string(spec);
  g_free(spec);
  pango_font_description_set_size(t->font_desc, font_size_pts * PANGO_SCALE);
  t->layout = gtk_widget_create_pango_layout(GTK_WIDGET(t), NULL);
  pango_layout_set_font_description(t->layout, t->font_desc);
  /* Compiled-in defaults only: the config's values arrive from the window,
   * which calls the two setters below for every pane it has just built. */
  t->report_mouse = PT_CONFIG_MOUSE_REPORTING_DEFAULT;
  t->osc52 = PT_CONFIG_OSC52_DEFAULT;
  t->blink_visible = TRUE;
  t->link_row = -2;          /* no cached link answer yet */
  live_terminals = g_slist_prepend(live_terminals, t);

  GtkEventController *key = gtk_event_controller_key_new();
  g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), t);
  g_signal_connect(key, "modifiers", G_CALLBACK(on_key_modifiers), t);
  gtk_widget_add_controller(GTK_WIDGET(t), key);

  GtkEventController *scroll =
      gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), t);
  gtk_widget_add_controller(GTK_WIDGET(t), scroll);

  GtkGesture *click = gtk_gesture_click_new();
  /* button 0: report middle and right clicks too, not just the primary. */
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
  g_signal_connect(click, "pressed", G_CALLBACK(on_click_pressed), t);
  g_signal_connect(click, "released", G_CALLBACK(on_click_released), t);
  g_signal_connect(click, "cancel", G_CALLBACK(on_click_cancel), t);
  gtk_widget_add_controller(GTK_WIDGET(t), GTK_EVENT_CONTROLLER(click));

  GtkEventController *motion = gtk_event_controller_motion_new();
  g_signal_connect(motion, "motion", G_CALLBACK(on_motion), t);
  g_signal_connect(motion, "enter", G_CALLBACK(on_motion_enter), t);
  g_signal_connect(motion, "leave", G_CALLBACK(on_motion_leave), t);
  gtk_widget_add_controller(GTK_WIDGET(t), motion);

  GtkGesture *drag = gtk_gesture_drag_new();
  g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), t);
  gtk_widget_add_controller(GTK_WIDGET(t), GTK_EVENT_CONTROLLER(drag));

  GtkEventController *focus = gtk_event_controller_focus_new();
  g_signal_connect(focus, "enter", G_CALLBACK(on_focus_enter), t);
  g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), t);
  gtk_widget_add_controller(GTK_WIDGET(t), focus);
}

void pt_terminal_set_spawn_env(PtTerminal *t, const char *const *env_pairs) {
  g_clear_pointer(&t->env, g_strfreev);
  if (env_pairs != NULL) t->env = g_strdupv((char **)env_pairs);
}

void pt_terminal_set_startup_input(PtTerminal *t, const char *line) {
  g_clear_pointer(&t->startup_input, g_free);
  if (line != NULL) t->startup_input = g_strdup(line);
}

GtkWidget *pt_terminal_new(const char *cwd) {
  PtTerminal *t = g_object_new(PT_TYPE_TERMINAL, NULL);
  t->start_cwd = g_strdup(cwd != NULL ? cwd : g_get_home_dir());
  return GTK_WIDGET(t);
}
