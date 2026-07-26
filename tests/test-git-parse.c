#include "pt-git-parse.h"

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

/* ---------- file list ---------- */
static void expect_file(GPtrArray *files, guint i, const char *xy,
                        const char *path) {
  g_assert_cmpuint(i, <, files->len);
  const PtGitFile *f = g_ptr_array_index(files, i);
  g_assert_cmpstr(f->xy, ==, xy);
  g_assert_cmpstr(f->path, ==, path);
}

static void test_files_kinds(void) {
  GPtrArray *files = pt_git_parse_files(
      "# branch.head main\n"
      "# branch.ab +0 -0\n"
      "1 .M N... 100644 100644 100644 aaa bbb src/main.c\n"
      "1 A. N... 000000 100644 100644 aaa bbb src/new.c\n"
      "1 .D N... 100644 100644 000000 aaa bbb src/gone.c\n"
      "1 MM N... 100644 100644 100644 aaa bbb src/both.c\n"
      "2 R. N... 100644 100644 100644 aaa bbb R100 src/new-name.c\tsrc/old.c\n"
      "u UU N... 100644 100644 100644 100644 aaa bbb ccc src/conflict.c\n"
      "? src/untracked.c\n");
  g_assert_cmpuint(files->len, ==, 7);
  expect_file(files, 0, "M", "src/main.c");
  expect_file(files, 1, "A", "src/new.c");
  expect_file(files, 2, "D", "src/gone.c");
  expect_file(files, 3, "MM", "src/both.c");
  /* renames show the new path, never "new -> old" */
  expect_file(files, 4, "R", "src/new-name.c");
  expect_file(files, 5, "UU", "src/conflict.c");
  expect_file(files, 6, "??", "src/untracked.c");
  g_ptr_array_unref(files);
}

/* Paths with spaces survive: the column split has to stop at the path. */
static void test_files_path_with_spaces(void) {
  GPtrArray *files = pt_git_parse_files(
      "1 .M N... 100644 100644 100644 aaa bbb docs/my notes.md\n"
      "? some dir/other file.txt\n");
  g_assert_cmpuint(files->len, ==, 2);
  expect_file(files, 0, "M", "docs/my notes.md");
  expect_file(files, 1, "??", "some dir/other file.txt");
  g_ptr_array_unref(files);
}

/* Not a repository (git printed nothing) and NULL both give an empty array. */
static void test_files_empty(void) {
  GPtrArray *files = pt_git_parse_files("");
  g_assert_cmpuint(files->len, ==, 0);
  g_ptr_array_unref(files);
  files = pt_git_parse_files("# branch.head main\n# branch.ab +0 -0\n");
  g_assert_cmpuint(files->len, ==, 0);
  g_ptr_array_unref(files);
  files = pt_git_parse_files(NULL);
  g_assert_nonnull(files);
  g_assert_cmpuint(files->len, ==, 0);
  g_ptr_array_unref(files);
}

/* The file count and PtGitStatus.changed come from the same lines. */
static void test_files_match_changed_count(void) {
  static const char *out =
      "# branch.head main\n"
      "1 .M N... 100644 100644 100644 aaa bbb a.c\n"
      "2 R. N... 100644 100644 100644 aaa bbb R100 b.c\tc.c\n"
      "u UU N... 100644 100644 100644 100644 aaa bbb ccc d.c\n"
      "? e.c\n";
  PtGitStatus s;
  g_assert_true(pt_git_parse_porcelain_v2(out, &s));
  GPtrArray *files = pt_git_parse_files(out);
  g_assert_cmpint(s.changed, ==, (int)files->len);
  g_ptr_array_unref(files);
}

/* ---------- numstat ---------- */
static void expect_stat(GPtrArray *stats, guint i, int add, int del,
                        const char *path) {
  g_assert_cmpuint(i, <, stats->len);
  const PtGitNumstat *s = g_ptr_array_index(stats, i);
  g_assert_cmpint(s->add, ==, add);
  g_assert_cmpint(s->del, ==, del);
  g_assert_cmpstr(s->path, ==, path);
}

static void test_numstat_counts(void) {
  GPtrArray *stats = pt_git_parse_numstat(
      "6\t0\tsrc/main.c\n"
      "121\t22\tsrc/style.css\n"
      "0\t0\tdocs/my notes.md\n");
  g_assert_cmpuint(stats->len, ==, 3);
  expect_stat(stats, 0, 6, 0, "src/main.c");
  expect_stat(stats, 1, 121, 22, "src/style.css");
  expect_stat(stats, 2, 0, 0, "docs/my notes.md");
  g_ptr_array_unref(stats);
}

/* Binary files print "-  -" and have no line counts to show. */
static void test_numstat_binary(void) {
  GPtrArray *stats = pt_git_parse_numstat("-\t-\tsrc/icon.png\n4\t1\ta.c\n");
  g_assert_cmpuint(stats->len, ==, 2);
  expect_stat(stats, 0, -1, -1, "src/icon.png");
  expect_stat(stats, 1, 4, 1, "a.c");
  g_ptr_array_unref(stats);
}

/* Renames factor the unchanged parts out; only the new path can match. */
static void test_numstat_renames(void) {
  GPtrArray *stats = pt_git_parse_numstat(
      "1\t1\told.c => new.c\n"
      "2\t3\tsrc/{old => new}.c\n"
      "4\t0\tdir/{ => sub}/f.c\n"
      "5\t2\tdir/{sub => }/g.c\n"
      "1\t0\t{a => b}/{c => d}.txt\n");
  g_assert_cmpuint(stats->len, ==, 5);
  expect_stat(stats, 0, 1, 1, "new.c");
  expect_stat(stats, 1, 2, 3, "src/new.c");
  expect_stat(stats, 2, 4, 0, "dir/sub/f.c");
  expect_stat(stats, 3, 5, 2, "dir/g.c");
  expect_stat(stats, 4, 1, 0, "b/d.txt");
  g_ptr_array_unref(stats);
}

static void test_numstat_empty(void) {
  GPtrArray *stats = pt_git_parse_numstat("");
  g_assert_cmpuint(stats->len, ==, 0);
  g_ptr_array_unref(stats);
  stats = pt_git_parse_numstat(NULL);
  g_assert_nonnull(stats);
  g_assert_cmpuint(stats->len, ==, 0);
  g_ptr_array_unref(stats);
}

/* The merge is by path: untracked files keep -1 (they are not in the diff),
 * and diff rows for paths git no longer reports are dropped. */
static void test_numstat_merge(void) {
  GPtrArray *files = pt_git_parse_files(
      "1 .M N... 100644 100644 100644 aaa bbb src/main.c\n"
      "1 .M N... 100644 100644 100644 aaa bbb src/icon.png\n"
      "2 R. N... 100644 100644 100644 aaa bbb R100 src/new.c\tsrc/old.c\n"
      "? src/untracked.c\n");
  GPtrArray *stats = pt_git_parse_numstat(
      "6\t0\tsrc/main.c\n"
      "-\t-\tsrc/icon.png\n"
      "2\t3\tsrc/{old => new}.c\n"
      "9\t9\tsrc/gone.c\n");
  pt_git_files_merge_numstat(files, stats);

  const PtGitFile *main_c = g_ptr_array_index(files, 0);
  g_assert_cmpint(main_c->add, ==, 6);
  g_assert_cmpint(main_c->del, ==, 0);
  const PtGitFile *icon = g_ptr_array_index(files, 1);
  g_assert_cmpint(icon->add, ==, -1);
  g_assert_cmpint(icon->del, ==, -1);
  const PtGitFile *renamed = g_ptr_array_index(files, 2);
  g_assert_cmpint(renamed->add, ==, 2);
  g_assert_cmpint(renamed->del, ==, 3);
  const PtGitFile *untracked = g_ptr_array_index(files, 3);
  g_assert_cmpint(untracked->add, ==, -1);
  g_assert_cmpint(untracked->del, ==, -1);
  g_assert_cmpuint(files->len, ==, 4);   /* src/gone.c added nothing */

  g_ptr_array_unref(stats);
  g_ptr_array_unref(files);
}

/* No diff at all (unborn HEAD: `git diff HEAD` fails, so nothing is merged). */
static void test_numstat_merge_absent(void) {
  GPtrArray *files = pt_git_parse_files("? a.c\n");
  pt_git_files_merge_numstat(files, NULL);
  const PtGitFile *f = g_ptr_array_index(files, 0);
  g_assert_cmpint(f->add, ==, -1);
  g_assert_cmpint(f->del, ==, -1);
  g_ptr_array_unref(files);
}

static void test_files_copy(void) {
  GPtrArray *files = pt_git_parse_files(
      "1 .M N... 100644 100644 100644 aaa bbb src/main.c\n");
  GPtrArray *stats = pt_git_parse_numstat("6\t0\tsrc/main.c\n");
  pt_git_files_merge_numstat(files, stats);
  g_ptr_array_unref(stats);
  GPtrArray *copy = pt_git_files_copy(files);
  g_ptr_array_unref(files);   /* the copy owns its own strings */
  g_assert_cmpuint(copy->len, ==, 1);
  expect_file(copy, 0, "M", "src/main.c");
  const PtGitFile *f = g_ptr_array_index(copy, 0);
  g_assert_cmpint(f->add, ==, 6);   /* counts survive the copy */
  g_assert_cmpint(f->del, ==, 0);
  g_ptr_array_unref(copy);
  copy = pt_git_files_copy(NULL);
  g_assert_cmpuint(copy->len, ==, 0);
  g_ptr_array_unref(copy);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/gitparse/clean", test_clean_repo);
  g_test_add_func("/gitparse/dirty", test_dirty_repo);
  g_test_add_func("/gitparse/detached", test_detached_head);
  g_test_add_func("/gitparse/null", test_null_input);
  g_test_add_func("/gitparse/files/kinds", test_files_kinds);
  g_test_add_func("/gitparse/files/spaces", test_files_path_with_spaces);
  g_test_add_func("/gitparse/files/empty", test_files_empty);
  g_test_add_func("/gitparse/files/count", test_files_match_changed_count);
  g_test_add_func("/gitparse/files/copy", test_files_copy);
  g_test_add_func("/gitparse/numstat/counts", test_numstat_counts);
  g_test_add_func("/gitparse/numstat/binary", test_numstat_binary);
  g_test_add_func("/gitparse/numstat/renames", test_numstat_renames);
  g_test_add_func("/gitparse/numstat/empty", test_numstat_empty);
  g_test_add_func("/gitparse/numstat/merge", test_numstat_merge);
  g_test_add_func("/gitparse/numstat/merge-absent", test_numstat_merge_absent);
  return g_test_run();
}
