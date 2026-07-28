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

/* The visible grid of a bare terminal, as pt_term_core_grid_text() renders it
 * for a live core. Lets a test compare grids without spawning anything. */
char *pt_term_grid_text_raw(GhosttyTerminal t);
