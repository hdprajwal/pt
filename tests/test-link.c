#include "pt-link.h"
#include <string.h>

/* The matcher answers "what link covers this byte", so every case names an
 * offset. `at` defaults to the middle of the expected match, which is what a
 * pointer sitting on a URL asks for; cases that care about an edge say so. */
static char *match_at(const char *line, gsize at) {
  PtLinkSpan span;
  if (!pt_link_find_at(line, at, &span)) return NULL;
  return g_strndup(line + span.start, span.end - span.start);
}

/* Hovering anywhere inside the URL finds it, and only the URL. */
static void expect(const char *line, const char *want) {
  const char *found = strstr(line, want);
  g_assert_nonnull(found);                 /* the case itself must be sane */
  gsize start = (gsize)(found - line);
  /* Every byte of the expected match must answer with the same span: a link
     you can only click in the middle is not a link. */
  for (gsize i = start; i < start + strlen(want); i++) {
    char *got = match_at(line, i);
    g_assert_cmpstr(got, ==, want);
    g_free(got);
  }
}

static void expect_none(const char *line, gsize at) {
  char *got = match_at(line, at);
  g_assert_null(got);
}

/* ---- the corpus, taken from ghostty's own url.zig test block so pt matches
   the reference implementation case for case (schemes pt refuses to open are
   covered separately below). ---- */

static void test_plain_urls(void) {
  expect("hello https://example.com world", "https://example.com");
  expect("also match http://example.com non-secure links",
         "http://example.com");
  expect("some file with https://google.com https://duckduckgo.com links.",
         "https://google.com");
  expect("some file with https://google.com https://duckduckgo.com links.",
         "https://duckduckgo.com");
}

/* Markdown and prose put URLs next to punctuation; the punctuation is not
   part of the address. */
static void test_trailing_punctuation(void) {
  expect("Link inside (https://example.com) parens", "https://example.com");
  expect("Link period https://example.com. More text.", "https://example.com");
  expect("Link trailing comma https://example.com, more text.",
         "https://example.com");
  expect("Link in double quotes \"https://example.com\" and more",
         "https://example.com");
  expect("Link in single quotes 'https://example.com' and more",
         "https://example.com");
}

/* Parens that belong to the address are kept — the wikipedia case. */
static void test_balanced_brackets(void) {
  expect("https://example.com/foo(bar) more", "https://example.com/foo(bar)");
  expect("https://example.com/foo(bar)baz more",
         "https://example.com/foo(bar)baz");
  expect("square brackets https://example.com/[foo] and more",
         "https://example.com/[foo]");
  expect("[13]:TooManyStatements: TempFile#assign_temp_file_to_entity has "
         "approx 7 statements [https://example.com/docs/Too-Many-Statements.md]",
         "https://example.com/docs/Too-Many-Statements.md");
}

static void test_query_and_fragment(void) {
  expect("match with query url https://example.com?query=1&other=2 and more "
         "text.", "https://example.com?query=1&other=2");
  expect("weird characters https://example.com/~user/?query=1&other=2#hash "
         "and more", "https://example.com/~user/?query=1&other=2#hash");
  expect("url with dashes [mode 2027](https://github.com/contour-terminal/"
         "terminal-unicode-core) for better unicode support",
         "https://github.com/contour-terminal/terminal-unicode-core");
}

/* The scheme starts the match wherever it appears, even glued to prose. */
static void test_scheme_mid_word(void) {
  expect("dot.http://example.com", "http://example.com");
}

/* What pt is actually for: a dev server's line. */
static void test_localhost(void) {
  expect("  \342\236\234  Local:   http://localhost:5173/",
         "http://localhost:5173/");
  expect("Listening on http://127.0.0.1:3000", "http://127.0.0.1:3000");
}

static void test_mailto_and_file(void) {
  expect("write to mailto:test@example.com today",
         "mailto:test@example.com");
  expect("see file:///tmp/build.log for details", "file:///tmp/build.log");
}

/* pt only opens http, https, file and mailto (pt_term_core_hyperlink_is_safe),
   so nothing else is offered as a link — an underline pt will not act on is a
   promise it cannot keep. This is the one deliberate departure from ghostty,
   whose scheme list is much longer. */
static void test_unopenable_schemes_are_not_links(void) {
  expect_none("connect with ssh://1.2.3.4 now", 15);
  expect_none("match ftp://example.com ftp links", 10);
  expect_none("match tel://+12123456789 phone numbers", 10);
  expect_none("run javascript:alert(1) never", 10);
  expect_none("git://example.com/repo.git", 4);
}

static void test_no_link_here(void) {
  expect_none("just some ordinary output with no address in it", 12);
  expect_none("", 0);
  expect_none("example.com without a scheme is not a link", 3);
  /* Past the end of the line, and the byte just after a match. */
  expect_none("hello https://example.com", 99);
  expect_none("(https://example.com) parens", 20);   /* the ')' itself */
}

static void test_null_line(void) {
  PtLinkSpan span;
  g_assert_false(pt_link_find_at(NULL, 0, &span));
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/link/plain-urls", test_plain_urls);
  g_test_add_func("/link/trailing-punctuation", test_trailing_punctuation);
  g_test_add_func("/link/balanced-brackets", test_balanced_brackets);
  g_test_add_func("/link/query-and-fragment", test_query_and_fragment);
  g_test_add_func("/link/scheme-mid-word", test_scheme_mid_word);
  g_test_add_func("/link/localhost", test_localhost);
  g_test_add_func("/link/mailto-and-file", test_mailto_and_file);
  g_test_add_func("/link/unopenable-schemes", test_unopenable_schemes_are_not_links);
  g_test_add_func("/link/no-link-here", test_no_link_here);
  g_test_add_func("/link/null-line", test_null_line);
  return g_test_run();
}
