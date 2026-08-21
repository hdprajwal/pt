#include "pt-path.h"

/* Every case goes through the one entry point; `home` is a parameter, so the
 * rule is tested without a machine's real home directory ever mattering. */
static const char *abbrev(const char *path, const char *home) {
  static char buf[256];
  g_strlcpy(buf, "poison", sizeof buf);   /* every path must overwrite this */
  pt_path_home_abbrev(path, home, buf, sizeof buf);
  return buf;
}

/* The home directory itself is the whole answer. */
static void test_exact_home(void) {
  g_assert_cmpstr(abbrev("/home/me", "/home/me"), ==, "~");
}

static void test_under_home(void) {
  g_assert_cmpstr(abbrev("/home/me/dev/foo", "/home/me"), ==, "~/dev/foo");
  /* A trailing slash on the path is kept, as it is part of the tail. */
  g_assert_cmpstr(abbrev("/home/me/", "/home/me"), ==, "~/");
}

/* Only a whole leading component matches: "/home/metoo" is somebody else. */
static void test_sibling_prefix(void) {
  g_assert_cmpstr(abbrev("/home/metoo", "/home/me"), ==, "/home/metoo");
  g_assert_cmpstr(abbrev("/home/metoo/dev", "/home/me"), ==,
                  "/home/metoo/dev");
}

/* A trailing slash on `home` (as an env var may carry) changes nothing. */
static void test_home_trailing_slash(void) {
  g_assert_cmpstr(abbrev("/home/me/dev/foo", "/home/me/"), ==, "~/dev/foo");
  g_assert_cmpstr(abbrev("/home/me", "/home/me///"), ==, "~");
  g_assert_cmpstr(abbrev("/home/metoo", "/home/me/"), ==, "/home/metoo");
}

static void test_outside_home(void) {
  g_assert_cmpstr(abbrev("/etc/passwd", "/home/me"), ==, "/etc/passwd");
  g_assert_cmpstr(abbrev("relative/dir", "/home/me"), ==, "relative/dir");
}

/* No path is nothing to show; no home is nothing to abbreviate against. */
static void test_null_and_empty(void) {
  g_assert_cmpstr(abbrev(NULL, "/home/me"), ==, "");
  g_assert_cmpstr(abbrev("/home/me/dev", NULL), ==, "/home/me/dev");
  g_assert_cmpstr(abbrev("/home/me/dev", ""), ==, "/home/me/dev");
  g_assert_cmpstr(abbrev("", "/home/me"), ==, "");
  /* Home "/" abbreviates nothing: every absolute path would become "~/…",
   * which reads worse than the path. */
  g_assert_cmpstr(abbrev("/etc", "/"), ==, "/etc");
}

/* A short buffer truncates and stays NUL-terminated, like g_strlcpy. */
static void test_truncates(void) {
  char buf[5];
  pt_path_home_abbrev("/home/me/dev/foo", "/home/me", buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "~/de");
  pt_path_home_abbrev("/etc/passwd", "/home/me", buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "/etc");
  /* One byte holds the terminator and nothing else — never a stray "~". */
  char one[1];
  pt_path_home_abbrev("/home/me/dev", "/home/me", one, sizeof one);
  g_assert_cmpstr(one, ==, "");
  /* Zero capacity writes nothing at all. */
  char none[1] = { 'x' };
  pt_path_home_abbrev("/home/me/dev", "/home/me", none, 0);
  g_assert_cmpint(none[0], ==, 'x');
}

/* Trailing slashes go, the root stays: this is what a project-path
 * comparison runs both its sides through. */
static void test_normalize(void) {
  char *a = pt_path_normalize("/home/me/dev/pt/");
  char *b = pt_path_normalize("/home/me/dev/pt");
  g_assert_cmpstr(a, ==, "/home/me/dev/pt");
  g_assert_cmpstr(b, ==, "/home/me/dev/pt");
  g_assert_cmpstr(a, ==, b);   /* the whole point: equal under comparison */
  g_free(a);
  g_free(b);

  char *many = pt_path_normalize("/tmp/a///");
  g_assert_cmpstr(many, ==, "/tmp/a");
  g_free(many);

  /* The root is all slash and must survive as itself. */
  char *root = pt_path_normalize("/");
  g_assert_cmpstr(root, ==, "/");
  g_free(root);
  /* A path that is only slashes is the root too. */
  char *slashes = pt_path_normalize("///");
  g_assert_cmpstr(slashes, ==, "/");
  g_free(slashes);

  char *plain = pt_path_normalize("/no/trailing/slash");
  g_assert_cmpstr(plain, ==, "/no/trailing/slash");
  g_free(plain);

  char *empty = pt_path_normalize("");
  g_assert_cmpstr(empty, ==, "");
  g_free(empty);

  g_assert_null(pt_path_normalize(NULL));
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/path/exact-home", test_exact_home);
  g_test_add_func("/path/under-home", test_under_home);
  g_test_add_func("/path/sibling-prefix", test_sibling_prefix);
  g_test_add_func("/path/home-trailing-slash", test_home_trailing_slash);
  g_test_add_func("/path/outside-home", test_outside_home);
  g_test_add_func("/path/null-and-empty", test_null_and_empty);
  g_test_add_func("/path/truncates", test_truncates);
  g_test_add_func("/path/normalize", test_normalize);
  return g_test_run();
}
