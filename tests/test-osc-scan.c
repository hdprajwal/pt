/* The OSC scanner is driven directly here: no pty, no main loop, just bytes
 * in and (code, payload) pairs out. Everything the consumers in #16..#19 rely
 * on — split sequences, both terminators, the cap, recovery — is pinned here. */
#include "pt-term-core-internal.h"
#include <string.h>

#define MAX_HITS 8

typedef struct {
  int n;
  int code[MAX_HITS];
  char *payload[MAX_HITS];
  gsize len[MAX_HITS];
} Hits;

static void collect(int code, const char *payload, gsize len, gpointer user) {
  Hits *h = user;
  g_assert_cmpint(h->n, <, MAX_HITS);
  /* The payload must be NUL-terminated as well as counted; consumers that
   * only want a string should not have to copy first. */
  g_assert_cmpint(payload[len], ==, '\0');
  h->code[h->n] = code;
  h->payload[h->n] = g_strndup(payload, len);
  h->len[h->n] = len;
  h->n++;
}

static void hits_clear(Hits *h) {
  for (int i = 0; i < h->n; i++) g_free(h->payload[i]);
  h->n = 0;
}

static void feed(PtOscScan *s, Hits *h, const char *str) {
  pt_osc_scan_feed(s, (const guint8 *)str, strlen(str), collect, h);
}

static void test_bel_terminator(void) {
  PtOscScan s = {0};
  Hits h = {0};
  feed(&s, &h, "\033]9;hello\007");
  g_assert_cmpint(h.n, ==, 1);
  g_assert_cmpint(h.code[0], ==, 9);
  g_assert_cmpstr(h.payload[0], ==, "hello");
  g_assert_cmpuint(h.len[0], ==, 5);
  hits_clear(&h);
  pt_osc_scan_clear(&s);
}

static void test_st_terminator(void) {
  PtOscScan s = {0};
  Hits h = {0};
  feed(&s, &h, "\033]9;hello\033\\");
  g_assert_cmpint(h.n, ==, 1);
  g_assert_cmpint(h.code[0], ==, 9);
  g_assert_cmpstr(h.payload[0], ==, "hello");
  hits_clear(&h);
  pt_osc_scan_clear(&s);
}

static void test_split_across_feeds(void) {
  PtOscScan s = {0};
  Hits h = {0};
  feed(&s, &h, "\033]9;he");
  g_assert_cmpint(h.n, ==, 0);
  feed(&s, &h, "ll");
  g_assert_cmpint(h.n, ==, 0);
  feed(&s, &h, "o\007");
  g_assert_cmpint(h.n, ==, 1);
  g_assert_cmpint(h.code[0], ==, 9);
  g_assert_cmpstr(h.payload[0], ==, "hello");
  hits_clear(&h);

  /* The ST terminator can be split down the middle too. */
  feed(&s, &h, "\033]777;notify;x\033");
  g_assert_cmpint(h.n, ==, 0);
  feed(&s, &h, "\\");
  g_assert_cmpint(h.n, ==, 1);
  g_assert_cmpint(h.code[0], ==, 777);
  g_assert_cmpstr(h.payload[0], ==, "notify;x");   /* only the first ';' splits */
  hits_clear(&h);
  pt_osc_scan_clear(&s);
}

static void test_over_cap_dropped_then_recovers(void) {
  PtOscScan s = {0};
  Hits h = {0};
  GString *big = g_string_new("\033]9;");
  for (gsize i = 0; i < PT_OSC_MAX + 64; i++) g_string_append_c(big, 'x');
  g_string_append_c(big, '\007');
  feed(&s, &h, big->str);
  g_string_free(big, TRUE);
  g_assert_cmpint(h.n, ==, 0);

  feed(&s, &h, "\033]9;after\007");
  g_assert_cmpint(h.n, ==, 1);
  g_assert_cmpstr(h.payload[0], ==, "after");
  hits_clear(&h);

  /* An over-cap sequence with the other terminator drops the same way. */
  big = g_string_new("\033]9;");
  for (gsize i = 0; i < PT_OSC_MAX + 64; i++) g_string_append_c(big, 'y');
  g_string_append(big, "\033\\");
  feed(&s, &h, big->str);
  g_string_free(big, TRUE);
  g_assert_cmpint(h.n, ==, 0);
  feed(&s, &h, "\033]9;again\007");
  g_assert_cmpint(h.n, ==, 1);
  g_assert_cmpstr(h.payload[0], ==, "again");
  hits_clear(&h);
  pt_osc_scan_clear(&s);
}

static void test_clipboard_gets_a_bigger_cap(void) {
  PtOscScan s = {0};
  Hits h = {0};
  /* OSC 52 carries whole clipboards, so a payload well past the 8K general
   * cap still has to arrive intact. */
  gsize body = PT_OSC_MAX * 2;
  GString *big = g_string_new("\033]52;c;");
  for (gsize i = 0; i < body; i++) g_string_append_c(big, 'A');
  g_string_append_c(big, '\007');
  feed(&s, &h, big->str);
  g_string_free(big, TRUE);
  g_assert_cmpint(h.n, ==, 1);
  g_assert_cmpint(h.code[0], ==, 52);
  g_assert_cmpuint(h.len[0], ==, body + 2);   /* "c;" plus the body */
  hits_clear(&h);

  /* But 52 is capped too, not unbounded. */
  big = g_string_new("\033]52;c;");
  for (gsize i = 0; i < PT_OSC_52_MAX + 64; i++) g_string_append_c(big, 'A');
  g_string_append_c(big, '\007');
  feed(&s, &h, big->str);
  g_string_free(big, TRUE);
  g_assert_cmpint(h.n, ==, 0);
  feed(&s, &h, "\033]9;fine\007");
  g_assert_cmpint(h.n, ==, 1);
  g_assert_cmpstr(h.payload[0], ==, "fine");
  hits_clear(&h);
  pt_osc_scan_clear(&s);
}

static void test_unterminated_does_not_merge(void) {
  PtOscScan s = {0};
  Hits h = {0};
  /* ESC that is not ST abandons the sequence in progress; the ESC ] that
   * follows must start clean rather than glue "first" onto "second". */
  feed(&s, &h, "\033]9;first\033]9;second\007");
  g_assert_cmpint(h.n, ==, 1);
  g_assert_cmpstr(h.payload[0], ==, "second");
  hits_clear(&h);

  /* Same across a feed boundary. */
  feed(&s, &h, "\033]9;dangling");
  feed(&s, &h, "\033]9;kept\007");
  g_assert_cmpint(h.n, ==, 1);
  g_assert_cmpstr(h.payload[0], ==, "kept");
  hits_clear(&h);

  /* ESC followed by something else entirely drops the sequence outright. */
  feed(&s, &h, "\033]9;lost\033[0m plain text\r\n");
  g_assert_cmpint(h.n, ==, 0);
  feed(&s, &h, "\033]9;ok\007");
  g_assert_cmpint(h.n, ==, 1);
  g_assert_cmpstr(h.payload[0], ==, "ok");
  hits_clear(&h);
  pt_osc_scan_clear(&s);
}

/* pt_term_core_reset() calls pt_osc_scan_clear() to unwedge a scanner that a
 * dying program left parked mid-payload, so the clear has to take the buffered
 * bytes with it and not just the state. This is the guard for that: the pty-
 * level test in test-term-core is a smoke test and cannot prove it, because
 * ESC ] restarts the scanner anyway (see test_unterminated_does_not_merge), so
 * a whole sequence arriving after a missed clear still dispatches correctly.
 * Only the terminator the dead program never sent tells the two apart. */
static void test_clear_drops_a_partial_payload(void) {
  PtOscScan s = {0};
  Hits h = {0};
  feed(&s, &h, "\033]9;stale-payload");      /* the program died right here */
  g_assert_cmpint(h.n, ==, 0);
  g_assert_nonnull(s.buf);                   /* really is holding the payload */

  pt_osc_scan_clear(&s);
  g_assert_null(s.buf);

  /* The BEL that was still owed. In ground state these are ordinary bytes;
     with the buffer still there they would dispatch "stale-payloadand-more". */
  feed(&s, &h, "and-more\007");
  g_assert_cmpint(h.n, ==, 0);

  /* And the scanner still works afterwards, from a standing start. */
  feed(&s, &h, "\033]9;fresh\007");
  g_assert_cmpint(h.n, ==, 1);
  g_assert_cmpint(h.code[0], ==, 9);
  g_assert_cmpstr(h.payload[0], ==, "fresh");
  hits_clear(&h);
  pt_osc_scan_clear(&s);
}

static void test_plain_text_is_free(void) {
  PtOscScan s = {0};
  Hits h = {0};
  feed(&s, &h, "the quick brown fox jumps over the lazy dog\r\n$ ls -la\r\n");
  g_assert_cmpint(h.n, ==, 0);
  /* Nothing in progress means nothing buffered: the scanner must not touch
   * the allocator for ordinary output. */
  g_assert_null(s.buf);
  pt_osc_scan_clear(&s);
}

static void test_bare_9c_is_payload(void) {
  PtOscScan s = {0};
  Hits h = {0};
  /* 0x9C is payload here, not a single-byte ST. libghostty's osc_string table
   * maps 0x20..0xFF to osc_put and that write lands after the "anywhere"
   * 0x9C => ground rule in genTable(), so the parser appends it too — and it
   * must, because inside an OSC it consumes raw bytes rather than decoded
   * codepoints, and 0x9C is a UTF-8 continuation byte. U+011C is C4 9C:
   * terminating on it would cut this payload in half. */
  feed(&s, &h, "\033]9;\304\234 arrived\007");
  g_assert_cmpint(h.n, ==, 1);
  g_assert_cmpint(h.code[0], ==, 9);
  g_assert_cmpstr(h.payload[0], ==, "\304\234 arrived");
  g_assert_cmpuint(h.len[0], ==, 10);
  hits_clear(&h);
  pt_osc_scan_clear(&s);
}

static void test_malformed_is_dropped(void) {
  PtOscScan s = {0};
  Hits h = {0};
  feed(&s, &h, "\033]hello;world\007");        /* non-numeric prefix */
  feed(&s, &h, "\033];empty\007");             /* no prefix at all */
  feed(&s, &h, "\033]123456;huge\007");        /* absurd code */
  feed(&s, &h, "\033[32mnot an osc\033[0m");   /* CSI, not OSC */
  g_assert_cmpint(h.n, ==, 0);

  /* A bare code with no ';' still dispatches, with an empty payload. */
  feed(&s, &h, "\033]9\007");
  g_assert_cmpint(h.n, ==, 1);
  g_assert_cmpint(h.code[0], ==, 9);
  g_assert_cmpstr(h.payload[0], ==, "");
  g_assert_cmpuint(h.len[0], ==, 0);
  hits_clear(&h);

  /* ESC ESC ] is still the start of a sequence. */
  feed(&s, &h, "\033\033]9;doubled\007");
  g_assert_cmpint(h.n, ==, 1);
  g_assert_cmpstr(h.payload[0], ==, "doubled");
  hits_clear(&h);
  pt_osc_scan_clear(&s);
}

static void test_interleaved_with_output(void) {
  PtOscScan s = {0};
  Hits h = {0};
  feed(&s, &h,
       "building\r\n\033]9;4;1;40\007step 2\r\n\033]777;notify;done\033\\bye\r\n");
  g_assert_cmpint(h.n, ==, 2);
  g_assert_cmpint(h.code[0], ==, 9);
  g_assert_cmpstr(h.payload[0], ==, "4;1;40");
  g_assert_cmpint(h.code[1], ==, 777);
  g_assert_cmpstr(h.payload[1], ==, "notify;done");
  hits_clear(&h);
  pt_osc_scan_clear(&s);
}

/* The scanner rides alongside the parser and must be invisible to it: the
 * same bytes through a terminal with and without the scanner running have to
 * leave the same grid behind. */
static char *run_terminal(const char *input, gsize len, PtOscScan *s, Hits *h) {
  GhosttyTerminal t = NULL;
  GhosttyTerminalOptions opts = { .cols = 80, .rows = 24, .max_scrollback = 100 };
  g_assert_cmpint(ghostty_terminal_new(NULL, &t, opts), ==, GHOSTTY_SUCCESS);
  /* Feed in small chunks so a sequence straddles a boundary either way. */
  for (gsize off = 0; off < len; off += 7) {
    gsize n = MIN((gsize)7, len - off);
    ghostty_terminal_vt_write(t, (const uint8_t *)input + off, n);
    if (s != NULL)
      pt_osc_scan_feed(s, (const guint8 *)input + off, n, collect, h);
  }
  char *text = pt_term_grid_text_raw(t);
  ghostty_terminal_free(t);
  return text;
}

static void test_stream_is_untouched(void) {
  const char input[] =
      "hello world\r\n"
      "\033]9;a notification\007"
      "\033]0;a title\033\\"
      "\033]52;c;aGVsbG8=\007"
      "second line\r\n"
      "\033[1;32mgreen\033[0m and \033]9;4;1;70\007done\r\n";
  gsize len = sizeof(input) - 1;
  char *copy = g_strndup(input, len);

  PtOscScan s = {0};
  Hits h = {0};
  char *scanned = run_terminal(input, len, &s, &h);
  char *plain = run_terminal(input, len, NULL, NULL);

  g_assert_cmpstr(scanned, ==, plain);
  g_assert_cmpint(memcmp(copy, input, len), ==, 0);   /* input left alone */
  g_assert_cmpint(h.n, ==, 4);                        /* and still saw them all */
  g_assert_cmpint(h.code[0], ==, 9);
  g_assert_cmpint(h.code[1], ==, 0);
  g_assert_cmpint(h.code[2], ==, 52);
  g_assert_cmpint(h.code[3], ==, 9);

  hits_clear(&h);
  pt_osc_scan_clear(&s);
  g_free(scanned);
  g_free(plain);
  g_free(copy);
}

/* ---- desktop notifications (OSC 9, OSC 777) ----
 *
 * Every case below is taken from ghostty's own Zig tests in
 * terminal/osc/parsers/osc9.zig and terminal/osc/parsers/rxvt_extension.zig,
 * so pt classifies the same bytes the same way. */

/* Classify a whole `ESC ] ... BEL` sequence: scan it, then hand the (code,
 * payload) pair the scanner produced to pt_osc_notification. */
static PtOscNotifyKind classify(const char *seq, PtOscNotification *out) {
  PtOscScan s = {0};
  Hits h = {0};
  feed(&s, &h, seq);
  g_assert_cmpint(h.n, ==, 1);           /* the sequence itself must be sound */
  PtOscNotifyKind k =
      pt_osc_notification(h.code[0], h.payload[0], h.len[0], out);
  /* `out` points into h.payload[0], so copy before the hits are freed. */
  if (out != NULL && k == PT_OSC_NOTIFY_SHOW) {
    out->title = g_strndup(out->title, out->title_len);
    out->body = g_strndup(out->body, out->body_len);
  }
  hits_clear(&h);
  pt_osc_scan_clear(&s);
  return k;
}

static void assert_notifies(const char *seq, const char *title,
                            const char *body) {
  PtOscNotification n = {0};
  g_assert_cmpint(classify(seq, &n), ==, PT_OSC_NOTIFY_SHOW);
  g_assert_cmpstr(n.title, ==, title);
  g_assert_cmpstr(n.body, ==, body);
  g_free((char *)n.title);
  g_free((char *)n.body);
}

static void assert_kind(const char *seq, PtOscNotifyKind kind) {
  g_assert_cmpint(classify(seq, NULL), ==, kind);
}

static void test_osc9_notification(void) {
  assert_notifies("\033]9;build done\007", "", "build done");
  assert_notifies("\033]9;H\007", "", "H");            /* one character */
  assert_notifies("\033]9;\007", "", "");              /* and none at all */
  /* The ST form is the same sequence; the scanner has already dealt with it. */
  assert_notifies("\033]9;build done\033\\", "", "build done");
  /* Semicolons past the first belong to the body, not to a title. */
  assert_notifies("\033]9;a;b;c\007", "", "a;b;c");
}

static void test_osc777_notification(void) {
  assert_notifies("\033]777;notify;Build;done\007", "Build", "done");
  /* Body keeps every semicolon after the second one. */
  assert_notifies("\033]777;notify;Build;a;b\007", "Build", "a;b");
  /* Empty title and empty body are both legal, so long as both ';' are there. */
  assert_notifies("\033]777;notify;;\007", "", "");
  assert_notifies("\033]777;notify;Build;\007", "Build", "");
}

static void test_osc777_needs_the_notify_extension(void) {
  /* OSC 777 is rxvt's extension slot; `notify` is the only one pt implements
   * and the name has to match exactly. */
  assert_kind("\033]777;something-else;a;b\007", PT_OSC_NOTIFY_NONE);
  assert_kind("\033]777;notifyx;a;b\007", PT_OSC_NOTIFY_NONE);
  assert_kind("\033]777;notif;a;b\007", PT_OSC_NOTIFY_NONE);
  assert_kind("\033]777;NOTIFY;a;b\007", PT_OSC_NOTIFY_NONE);
  /* Missing the second ';' means no title, which ghostty calls malformed
   * rather than treating the rest as a body. */
  assert_kind("\033]777;notify;Build\007", PT_OSC_NOTIFY_NONE);
  assert_kind("\033]777;notify\007", PT_OSC_NOTIFY_NONE);
}

/* OSC 9 is shared with ConEmu. A payload that matches one of its extensions is
 * not a notification; a payload that only looks like one still is. */
static void test_osc9_conemu_extensions_are_not_notifications(void) {
  assert_kind("\033]9;1;500\007", PT_OSC_NOTIFY_NONE);      /* sleep */
  assert_kind("\033]9;2;hello\007", PT_OSC_NOTIFY_NONE);    /* message box */
  assert_kind("\033]9;3;title\007", PT_OSC_NOTIFY_NONE);    /* tab title */
  assert_kind("\033]9;3;\007", PT_OSC_NOTIFY_NONE);         /* tab title reset */
  assert_kind("\033]9;5\007", PT_OSC_NOTIFY_NONE);          /* wait for input */
  assert_kind("\033]9;5abc\007", PT_OSC_NOTIFY_NONE);       /* trailing ignored */
  assert_kind("\033]9;6;macro\007", PT_OSC_NOTIFY_NONE);    /* guimacro */
  assert_kind("\033]9;7;cmd\007", PT_OSC_NOTIFY_NONE);      /* run process */
  assert_kind("\033]9;8;VAR\007", PT_OSC_NOTIFY_NONE);      /* env var */
  assert_kind("\033]9;9;/home/me\007", PT_OSC_NOTIFY_NONE); /* report cwd */
  assert_kind("\033]9;10\007", PT_OSC_NOTIFY_NONE);         /* xterm emulation */
  assert_kind("\033]9;10;0\007", PT_OSC_NOTIFY_NONE);
  assert_kind("\033]9;10;3\007", PT_OSC_NOTIFY_NONE);
  assert_kind("\033]9;11;note\007", PT_OSC_NOTIFY_NONE);    /* comment */
  assert_kind("\033]9;11;\007", PT_OSC_NOTIFY_NONE);
  assert_kind("\033]9;12\007", PT_OSC_NOTIFY_NONE);         /* mark prompt */
  assert_kind("\033]9;12;abc\007", PT_OSC_NOTIFY_NONE);     /* takes no argument */

  /* Cut short or out of range, and it is an ordinary notification whose body
   * happens to start with a digit — ghostty pins each of these. */
  assert_notifies("\033]9;1\007", "", "1");
  assert_notifies("\033]9;1a\007", "", "1a");
  assert_notifies("\033]9;10;4\007", "", "10;4");
  assert_notifies("\033]9;10;\007", "", "10;");
  assert_notifies("\033]9;10;abc\007", "", "10;abc");
  assert_notifies("\033]9;11\007", "", "11");
  assert_notifies("\033]9;2\007", "", "2");
  assert_notifies("\033]9;9\007", "", "9");
}

/* The one case the issue calls out by name: `9;4;...` is a ConEmu progress
 * report and must never become a notification. It comes back named so the
 * progress work (#17) has a seam to attach to. */
static void test_osc9_progress_is_not_a_notification(void) {
  assert_kind("\033]9;4;1;40\007", PT_OSC_NOTIFY_PROGRESS);   /* set 40% */
  assert_kind("\033]9;4;0\007", PT_OSC_NOTIFY_PROGRESS);      /* remove */
  assert_kind("\033]9;4;1\007", PT_OSC_NOTIFY_PROGRESS);
  assert_kind("\033]9;4;2;10\007", PT_OSC_NOTIFY_PROGRESS);   /* error */
  assert_kind("\033]9;4;3\007", PT_OSC_NOTIFY_PROGRESS);      /* indeterminate */
  assert_kind("\033]9;4;4;50\007", PT_OSC_NOTIFY_PROGRESS);   /* pause */

  /* Ghostty's four fall-through cases: a `4` that never becomes a real
   * progress report is a notification, body and all. */
  assert_notifies("\033]9;4\007", "", "4");
  assert_notifies("\033]9;4;\007", "", "4;");
  assert_notifies("\033]9;4;5\007", "", "4;5");
  assert_notifies("\033]9;4;5a\007", "", "4;5a");
}

static void test_other_codes_are_never_notifications(void) {
  PtOscNotification n = {0};
  g_assert_cmpint(pt_osc_notification(0, "a title", 7, &n), ==,
                  PT_OSC_NOTIFY_NONE);
  g_assert_cmpint(pt_osc_notification(52, "c;aGk=", 6, &n), ==,
                  PT_OSC_NOTIFY_NONE);
  g_assert_cmpint(pt_osc_notification(8, ";https://example.com", 20, &n), ==,
                  PT_OSC_NOTIFY_NONE);
  g_assert_cmpint(pt_osc_notification(99, "", 0, &n), ==, PT_OSC_NOTIFY_NONE);
}

/* One per second whatever it says, and five before the same text repeats —
 * ghostty's two numbers. Driven with a synthetic clock so nothing sleeps. */
static void test_notify_gate_rate_limits(void) {
  pt_notify_gate_reset();
  gint64 t0 = 1000 * G_USEC_PER_SEC;   /* any origin; the gate is relative */

  g_assert_true(pt_notify_gate(t0, "", "first"));
  /* A hundred in the same instant, all different, all but the first refused. */
  for (int i = 0; i < 100; i++) {
    char *body = g_strdup_printf("body %d", i);
    g_assert_false(pt_notify_gate(t0, "", body));
    g_free(body);
  }
  /* Just under a second is still too soon; a second later is fine. */
  g_assert_false(pt_notify_gate(t0 + G_USEC_PER_SEC - 1, "", "second"));
  g_assert_true(pt_notify_gate(t0 + G_USEC_PER_SEC, "", "second"));

  /* A refused notification must not push the window forward, or a tight
   * enough loop would keep the gate shut forever. */
  pt_notify_gate_reset();
  g_assert_true(pt_notify_gate(t0, "", "a"));
  for (gint64 us = t0; us < t0 + G_USEC_PER_SEC; us += 1000)
    pt_notify_gate(us, "", "spam");
  g_assert_true(pt_notify_gate(t0 + G_USEC_PER_SEC, "", "b"));
}

static void test_notify_gate_suppresses_repeats(void) {
  pt_notify_gate_reset();
  gint64 t0 = 1000 * G_USEC_PER_SEC;
  g_assert_true(pt_notify_gate(t0, "Build", "done"));
  /* Past the one-second limit, but the same text inside five seconds. */
  g_assert_false(pt_notify_gate(t0 + 2 * G_USEC_PER_SEC, "Build", "done"));
  /* Different text at the same moment goes through. */
  g_assert_true(pt_notify_gate(t0 + 2 * G_USEC_PER_SEC, "Build", "failed"));
  /* And the repeat is allowed once the five seconds are up. */
  g_assert_true(pt_notify_gate(t0 + 8 * G_USEC_PER_SEC, "Build", "failed"));
  pt_notify_gate_reset();
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/oscscan/bel", test_bel_terminator);
  g_test_add_func("/oscscan/st", test_st_terminator);
  g_test_add_func("/oscscan/split", test_split_across_feeds);
  g_test_add_func("/oscscan/over-cap", test_over_cap_dropped_then_recovers);
  g_test_add_func("/oscscan/clipboard-cap", test_clipboard_gets_a_bigger_cap);
  g_test_add_func("/oscscan/unterminated", test_unterminated_does_not_merge);
  g_test_add_func("/oscscan/clear-drops-partial",
                  test_clear_drops_a_partial_payload);
  g_test_add_func("/oscscan/plain-text", test_plain_text_is_free);
  g_test_add_func("/oscscan/bare-9c", test_bare_9c_is_payload);
  g_test_add_func("/oscscan/malformed", test_malformed_is_dropped);
  g_test_add_func("/oscscan/interleaved", test_interleaved_with_output);
  g_test_add_func("/oscscan/stream-untouched", test_stream_is_untouched);
  g_test_add_func("/oscnotify/osc9", test_osc9_notification);
  g_test_add_func("/oscnotify/osc777", test_osc777_notification);
  g_test_add_func("/oscnotify/osc777-extension",
                  test_osc777_needs_the_notify_extension);
  g_test_add_func("/oscnotify/conemu",
                  test_osc9_conemu_extensions_are_not_notifications);
  g_test_add_func("/oscnotify/progress",
                  test_osc9_progress_is_not_a_notification);
  g_test_add_func("/oscnotify/other-codes",
                  test_other_codes_are_never_notifications);
  g_test_add_func("/oscnotify/gate-rate-limit", test_notify_gate_rate_limits);
  g_test_add_func("/oscnotify/gate-repeats", test_notify_gate_suppresses_repeats);
  return g_test_run();
}
