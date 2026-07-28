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

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/oscscan/bel", test_bel_terminator);
  g_test_add_func("/oscscan/st", test_st_terminator);
  g_test_add_func("/oscscan/split", test_split_across_feeds);
  g_test_add_func("/oscscan/over-cap", test_over_cap_dropped_then_recovers);
  g_test_add_func("/oscscan/clipboard-cap", test_clipboard_gets_a_bigger_cap);
  g_test_add_func("/oscscan/unterminated", test_unterminated_does_not_merge);
  g_test_add_func("/oscscan/plain-text", test_plain_text_is_free);
  g_test_add_func("/oscscan/malformed", test_malformed_is_dropped);
  g_test_add_func("/oscscan/interleaved", test_interleaved_with_output);
  g_test_add_func("/oscscan/stream-untouched", test_stream_is_untouched);
  return g_test_run();
}
