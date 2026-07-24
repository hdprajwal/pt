#include "pt-git-parse.h"
#include <string.h>

static void test_clean_repo(void) {
  PtGitStatus s;
  g_assert_true(pt_git_parse_porcelain_v2(
      "# branch.oid 1234abcd\n"
      "# branch.head main\n"
      "# branch.upstream origin/main\n"
      "# branch.ab +0 -0\n", &s));
  g_assert_cmpstr(s.branch, ==, "main");
  g_assert_cmpint(s.changed, ==, 0);
  g_assert_cmpint(s.ahead, ==, 0);
  g_assert_cmpint(s.behind, ==, 0);
}

static void test_dirty_repo(void) {
  PtGitStatus s;
  g_assert_true(pt_git_parse_porcelain_v2(
      "# branch.head feature/x\n"
      "# branch.ab +2 -1\n"
      "1 .M N... 100644 100644 100644 aaa bbb src/main.c\n"
      "2 R. N... 100644 100644 100644 aaa bbb R100 new.c\told.c\n"
      "u UU N... 100644 100644 100644 100644 aaa bbb ccc conflict.c\n"
      "? untracked.c\n", &s));
  g_assert_cmpstr(s.branch, ==, "feature/x");
  g_assert_cmpint(s.changed, ==, 4);
  g_assert_cmpint(s.ahead, ==, 2);
  g_assert_cmpint(s.behind, ==, 1);
}

static void test_detached_head(void) {
  PtGitStatus s;
  g_assert_true(pt_git_parse_porcelain_v2("# branch.head (detached)\n", &s));
  g_assert_cmpstr(s.branch, ==, "(detached)");
}

static void test_null_input(void) {
  PtGitStatus s;
  g_assert_false(pt_git_parse_porcelain_v2(NULL, &s));
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/gitparse/clean", test_clean_repo);
  g_test_add_func("/gitparse/dirty", test_dirty_repo);
  g_test_add_func("/gitparse/detached", test_detached_head);
  g_test_add_func("/gitparse/null", test_null_input);
  return g_test_run();
}
