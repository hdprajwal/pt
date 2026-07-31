#pragma once
#include <glib.h>

/* ---- bare URLs in plain text ----
 *
 * OSC 8 covers the links a program declares; this covers the ones it just
 * prints — a dev server's `http://localhost:5173/`, a URL in a stack trace,
 * an address in someone's output. libghostty knows nothing about these: they
 * are ordinary text, and the only thing that makes them a link is a match
 * against the pattern below.
 *
 * The pattern is ghostty's (src/config/url.zig, the scheme-URL branch), with
 * its scheme list narrowed to what pt is willing to open. Underlining an
 * address pt then refuses to launch is a promise it cannot keep, so `ssh:`
 * and friends are not offered at all — that is the one deliberate departure.
 *
 * Byte offsets into the line, half-open, exactly as g_regex reports them. */
typedef struct {
  gsize start;
  gsize end;
} PtLinkSpan;

/* The URL covering byte `at` of `line`, if any. Answers for every byte of the
 * match, since a pointer lands wherever it lands: a link only clickable in the
 * middle is not a link. `line` must be valid UTF-8 (a terminal row rendered by
 * the core always is). NULL, an empty line or an offset past the end report
 * no match and leave `*out` untouched. */
gboolean pt_link_find_at(const char *line, gsize at, PtLinkSpan *out);
