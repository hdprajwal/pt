#pragma once
/* Internals of pt-term-core.c, exposed only so tests can drive them without a
 * pty and a main loop. Nothing outside pt-term-core.c and tests/ includes this;
 * the API the rest of pt uses is pt-term-core.h. */
#include <glib.h>
#include <ghostty/vt.h>

/* ---- OSC scanner ----
 *
 * libghostty parses OSC but hands almost none of it back: its data accessor
 * only exposes the window title, so an OSC 9 arrives and nothing says what it
 * said. So pt runs its own scanner over the same bytes, alongside the parser
 * rather than in front of it — it consumes nothing, writes nothing to the pty
 * and never modifies the stream. Codes pt does not care about are dropped,
 * including the ones libghostty already handles.
 *
 * Sequences split across reads are normal (a read can end anywhere), so the
 * state lives on the caller's PtOscScan, not on the stack. */

/* Everything pt cares about fits in 8K many times over — except OSC 52, which
 * carries an entire clipboard. Payloads past their cap are dropped, not grown
 * into: an app that never terminates its OSC must not be able to grow a pane's
 * memory without limit. */
#define PT_OSC_MAX     (8u * 1024u)
#define PT_OSC_52_MAX  (1024u * 1024u)
/* And the same megabyte again on the far side of the decode. The two caps do
 * not stack: PT_OSC_52_MAX bounds the base64 *as it arrives*, and base64 is
 * four characters per three bytes, so a sequence that clears the scanner can
 * never decode to more than ~768K. This one is the backstop for a payload that
 * reaches the decoder another way — it is checked against the encoded length,
 * before anything is allocated, so an oversized clipboard costs nothing. */
#define PT_OSC_52_TEXT_MAX  (1024u * 1024u)

typedef enum {
  PT_OSC_GROUND = 0,      /* outside a sequence; the overwhelmingly common case */
  PT_OSC_ESC,             /* saw ESC, waiting to see whether ']' follows */
  PT_OSC_PAYLOAD,         /* inside a sequence, accumulating */
  PT_OSC_PAYLOAD_ESC,     /* saw ESC inside a sequence; ST if '\\' follows */
  PT_OSC_DROP,            /* over the cap: track to the terminator, keep nothing */
  PT_OSC_DROP_ESC,
} PtOscState;

typedef struct {
  PtOscState state;
  GString *buf;           /* NULL until the first sequence starts */
} PtOscScan;

/* payload is NUL-terminated as well as counted, and is only valid for the
 * duration of the call. */
typedef void (*PtOscScanFn)(int code, const char *payload, gsize len,
                            gpointer user);

/* Feed one read's worth of pty output. Calls fn once per complete, well-formed
 * sequence; malformed and over-cap ones are dropped silently. */
void pt_osc_scan_feed(PtOscScan *s, const guint8 *data, gsize len,
                      PtOscScanFn fn, gpointer user);
/* Release the buffer and return to the ground state. */
void pt_osc_scan_clear(PtOscScan *s);

/* ---- desktop notifications (OSC 9, OSC 777) ----
 *
 * Two codes carry the same thing. `ESC ] 9 ; body BEL` is iTerm2's, and has no
 * title; `ESC ] 777 ; notify ; title ; body BEL` is rxvt's, and has both.
 *
 * OSC 9 is shared with ConEmu, which hangs a dozen unrelated extensions off
 * subcodes (`9;1;` sleep, `9;4;` progress, `9;9;` cwd, ...), so telling a
 * notification from one of those is the whole job here. The rule replicated
 * below is ghostty's, byte for byte, from
 * terminal/osc/parsers/osc9.zig: a payload is a ConEmu extension only when it
 * matches one exactly, and *anything* else — including a truncated or
 * misspelled extension — is a notification whose body is the whole payload.
 * That is why `9;4` and `9;4;` are notifications while `9;4;1` is a progress
 * report; ghostty's own tests pin each of those cases.
 *
 * Progress reports come back named rather than merged into "not a
 * notification", so the ConEmu progress work (issue #17) has somewhere to
 * attach without re-deciding any of this. */
typedef enum {
  PT_OSC_NOTIFY_NONE = 0,  /* a ConEmu extension pt does not implement */
  PT_OSC_NOTIFY_SHOW,      /* raise it: `out` is filled in */
  PT_OSC_NOTIFY_PROGRESS,  /* ConEmu `9;4;...` progress report — issue #17 */
} PtOscNotifyKind;

/* Both halves point into `payload` and are valid only as long as it is.
 * Neither is NUL-terminated (the body of an OSC 777 ends where the payload
 * does, but the title does not), so both come with a length. `title` is the
 * empty string for OSC 9, which never carries one. */
typedef struct {
  const char *title;
  gsize title_len;
  const char *body;
  gsize body_len;
} PtOscNotification;

/* `code` and `payload` straight from the scanner. Answers NONE for every code
 * that is not 9 or 777, so callers can hand it everything. */
PtOscNotifyKind pt_osc_notification(int code, const char *payload, gsize len,
                                    PtOscNotification *out);

/* Ghostty's caps, from the fixed-size message it posts to the apprt
 * (apprt/surface.zig: `title: [63:0]u8`, `body: [255:0]u8`). Over-long text is
 * truncated rather than dropped — a build log line that runs long should still
 * notify. pt truncates on a character boundary where ghostty cuts mid-byte:
 * the text leaves here for a GNotification, i.e. for the session bus, and half
 * a codepoint is not something a D-Bus string may carry. */
#define PT_NOTIFY_TITLE_MAX  63
#define PT_NOTIFY_BODY_MAX  255

/* The rate limit, process-wide because ghostty's is too: it lives on the App
 * and not on the Surface (Surface.zig showDesktopNotification), so one pane in
 * a loop cannot drown out every other pane. One per second, plus a longer
 * window in which the same text cannot repeat at all.
 *
 * `now_us` is g_get_monotonic_time() in the app and a synthetic clock in the
 * tests. Returns TRUE when the notification may be shown, and only then
 * records it — a suppressed notification does not push the window forward,
 * or a fast enough loop would suppress everything forever. */
gboolean pt_notify_gate(gint64 now_us, const char *title, const char *body);
void pt_notify_gate_reset(void);   /* tests only: the state is process-wide */

/* ---- OSC 52 payloads ----
 *
 * `<targets>;<base64>`, as it arrives from the scanner. Returns the decoded
 * text (caller g_free's it, NUL-terminated, *out_len bytes) and sets *primary
 * when the program asked for the primary selection instead of the clipboard.
 *
 * NULL for everything else, and there is a lot of everything else: a read
 * request (`?` in place of the data), a payload with no ';' in it, text that
 * is not valid base64 — g_base64_decode() answers junk rather than failing, so
 * the alphabet is checked first — an empty clipboard, one over the cap, or one
 * whose decoded bytes contain a NUL or are not valid UTF-8. */
char *pt_osc52_decode(const char *payload, gsize len, gboolean *primary,
                      gsize *out_len);

/* The visible grid of a bare terminal, as pt_term_core_grid_text() renders it
 * for a live core. Lets a test compare grids without spawning anything. */
char *pt_term_grid_text_raw(GhosttyTerminal t);

/* ---- scrollbar reads ----
 *
 * How many times pt_term_core_scrollbar() has gone to the library rather than
 * answering from its cache. The library warns the query is expensive, so the
 * count is the only way to prove the cache is doing its job; nothing outside
 * tests/ has any business reading it. */
typedef struct PtTermCore PtTermCore;
guint64 pt_term_core_scrollbar_reads(PtTermCore *c);
