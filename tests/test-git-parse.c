#include "pt-git-parse.h"

/* Every test goes through the one entry point: status text (plus optional
 * numstat text) in, status struct and owned file array out. */
static GPtrArray *parse(const char *status_text, const char *numstat_text,
                        PtGitStatus *s) {
  GPtrArray *files = NULL;
  pt_git_result_parse(status_text, numstat_text, s, &files);
  g_assert_nonnull(files);
  return files;
}

static void expect_file(GPtrArray *files, guint i, const char *xy,
                        const char *path) {
  g_assert_cmpuint(i, <, files->len);
  const PtGitFile *f = g_ptr_array_index(files, i);
  g_assert_cmpstr(f->xy, ==, xy);
  g_assert_cmpstr(f->path, ==, path);
}

static void expect_counts(GPtrArray *files, guint i, int add, int del) {
  g_assert_cmpuint(i, <, files->len);
  const PtGitFile *f = g_ptr_array_index(files, i);
  g_assert_cmpint(f->add, ==, add);
  g_assert_cmpint(f->del, ==, del);
}

static void test_clean_repo(void) {
  PtGitStatus s;
  GPtrArray *files = parse(
      "# branch.oid 1234abcd\n"
      "# branch.head main\n"
      "# branch.upstream origin/main\n"
      "# branch.ab +0 -0\n", NULL, &s);
  g_assert_cmpstr(s.branch, ==, "main");
  g_assert_cmpint(s.changed, ==, 0);
  g_assert_cmpint(s.ahead, ==, 0);
  g_assert_cmpint(s.behind, ==, 0);
  g_assert_cmpuint(files->len, ==, 0);
  g_ptr_array_unref(files);
}

static void test_dirty_repo(void) {
  PtGitStatus s;
  GPtrArray *files = parse(
      "# branch.head feature/x\n"
      "# branch.ab +2 -1\n"
      "1 .M N... 100644 100644 100644 aaa bbb src/main.c\n"
      "2 R. N... 100644 100644 100644 aaa bbb R100 new.c\told.c\n"
      "u UU N... 100644 100644 100644 100644 aaa bbb ccc conflict.c\n"
      "? untracked.c\n", NULL, &s);
  g_assert_cmpstr(s.branch, ==, "feature/x");
  g_assert_cmpint(s.ahead, ==, 2);
  g_assert_cmpint(s.behind, ==, 1);
  /* `changed` is the file list's length by construction */
  g_assert_cmpint(s.changed, ==, 4);
  g_assert_cmpuint(files->len, ==, 4);
  g_ptr_array_unref(files);
}

static void test_detached_head(void) {
  PtGitStatus s;
  GPtrArray *files = parse("# branch.head (detached)\n", NULL, &s);
  g_assert_cmpstr(s.branch, ==, "(detached)");
  g_ptr_array_unref(files);
}

/* NULL status text (not a repo, git absent) gives a zeroed status and an
 * empty — never NULL — file array. */
static void test_null_input(void) {
  PtGitStatus s;
  GPtrArray *files = parse(NULL, NULL, &s);
  g_assert_cmpstr(s.branch, ==, "");
  g_assert_cmpint(s.changed, ==, 0);
  g_assert_cmpuint(files->len, ==, 0);
  g_ptr_array_unref(files);
}

static void test_files_kinds(void) {
  PtGitStatus s;
  GPtrArray *files = parse(
      "# branch.head main\n"
      "# branch.ab +0 -0\n"
      "1 .M N... 100644 100644 100644 aaa bbb src/main.c\n"
      "1 A. N... 000000 100644 100644 aaa bbb src/new.c\n"
      "1 .D N... 100644 100644 000000 aaa bbb src/gone.c\n"
      "1 MM N... 100644 100644 100644 aaa bbb src/both.c\n"
      "2 R. N... 100644 100644 100644 aaa bbb R100 src/new-name.c\tsrc/old.c\n"
      "u UU N... 100644 100644 100644 100644 aaa bbb ccc src/conflict.c\n"
      "? src/untracked.c\n", NULL, &s);
  g_assert_cmpuint(files->len, ==, 7);
  g_assert_cmpint(s.changed, ==, 7);
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

/* Rename entries ("2 ...") carry "<new>\t<orig>"; the display path is the new
 * one — the first of the tab pair — and spaces in either survive. */
static void test_files_renames(void) {
  PtGitStatus s;
  GPtrArray *files = parse(
      "2 R. N... 100644 100644 100644 aaa bbb R100 src/after.c\tsrc/before.c\n"
      "2 RM N... 100644 100644 100644 aaa bbb R090 "
          "docs/new name.md\tdocs/old name.md\n", NULL, &s);
  g_assert_cmpuint(files->len, ==, 2);
  expect_file(files, 0, "R", "src/after.c");
  expect_file(files, 1, "RM", "docs/new name.md");
  g_ptr_array_unref(files);
}

/* Paths with spaces survive: the column scan has to stop at the path. */
static void test_files_path_with_spaces(void) {
  PtGitStatus s;
  GPtrArray *files = parse(
      "1 .M N... 100644 100644 100644 aaa bbb docs/my notes.md\n"
      "? some dir/other file.txt\n", NULL, &s);
  g_assert_cmpuint(files->len, ==, 2);
  expect_file(files, 0, "M", "docs/my notes.md");
  expect_file(files, 1, "??", "some dir/other file.txt");
  g_ptr_array_unref(files);
}

/* Not a repository (git printed nothing) and a clean status both give an
 * empty array and changed == 0. */
static void test_files_empty(void) {
  PtGitStatus s;
  GPtrArray *files = parse("", NULL, &s);
  g_assert_cmpuint(files->len, ==, 0);
  g_assert_cmpint(s.changed, ==, 0);
  g_ptr_array_unref(files);
  files = parse("# branch.head main\n# branch.ab +0 -0\n", NULL, &s);
  g_assert_cmpuint(files->len, ==, 0);
  g_ptr_array_unref(files);
}

/* ---------- numstat merged in the same call ---------- */
static void test_numstat_counts(void) {
  PtGitStatus s;
  GPtrArray *files = parse(
      "1 .M N... 100644 100644 100644 aaa bbb src/main.c\n"
      "1 .M N... 100644 100644 100644 aaa bbb src/style.css\n"
      "1 .M N... 100644 100644 100644 aaa bbb docs/my notes.md\n",
      "6\t0\tsrc/main.c\n"
      "121\t22\tsrc/style.css\n"
      "0\t0\tdocs/my notes.md\n", &s);
  g_assert_cmpuint(files->len, ==, 3);
  expect_counts(files, 0, 6, 0);
  expect_counts(files, 1, 121, 22);
  expect_counts(files, 2, 0, 0);
  g_ptr_array_unref(files);
}

/* Binary files print "-  -" and have no line counts to show. */
static void test_numstat_binary(void) {
  PtGitStatus s;
  GPtrArray *files = parse(
      "1 .M N... 100644 100644 100644 aaa bbb src/icon.png\n"
      "1 .M N... 100644 100644 100644 aaa bbb a.c\n",
      "-\t-\tsrc/icon.png\n4\t1\ta.c\n", &s);
  expect_counts(files, 0, -1, -1);
  expect_counts(files, 1, 4, 1);
  g_ptr_array_unref(files);
}

/* Numstat renames factor the unchanged parts out ("old => new",
 * "src/{old => new}.c", …); only the rebuilt new path can match a file. */
static void test_numstat_renames(void) {
  PtGitStatus s;
  GPtrArray *files = parse(
      "1 .M N... 100644 100644 100644 aaa bbb new.c\n"
      "1 .M N... 100644 100644 100644 aaa bbb src/new.c\n"
      "1 .M N... 100644 100644 100644 aaa bbb dir/sub/f.c\n"
      "1 .M N... 100644 100644 100644 aaa bbb dir/g.c\n"
      "1 .M N... 100644 100644 100644 aaa bbb b/d.txt\n",
      "1\t1\told.c => new.c\n"
      "2\t3\tsrc/{old => new}.c\n"
      "4\t0\tdir/{ => sub}/f.c\n"
      "5\t2\tdir/{sub => }/g.c\n"
      "1\t0\t{a => b}/{c => d}.txt\n", &s);
  g_assert_cmpuint(files->len, ==, 5);
  expect_counts(files, 0, 1, 1);
  expect_counts(files, 1, 2, 3);
  expect_counts(files, 2, 4, 0);
  expect_counts(files, 3, 5, 2);
  expect_counts(files, 4, 1, 0);
  g_ptr_array_unref(files);
}

/* The merge is by path: untracked files keep -1 (they are not in the diff),
 * a porcelain rename matches its numstat row through the new path, and diff
 * rows for paths git no longer reports are dropped. */
static void test_numstat_merge(void) {
  PtGitStatus s;
  GPtrArray *files = parse(
      "1 .M N... 100644 100644 100644 aaa bbb src/main.c\n"
      "1 .M N... 100644 100644 100644 aaa bbb src/icon.png\n"
      "2 R. N... 100644 100644 100644 aaa bbb R100 src/new.c\tsrc/old.c\n"
      "? src/untracked.c\n",
      "6\t0\tsrc/main.c\n"
      "-\t-\tsrc/icon.png\n"
      "2\t3\tsrc/{old => new}.c\n"
      "9\t9\tsrc/gone.c\n", &s);
  expect_counts(files, 0, 6, 0);
  expect_counts(files, 1, -1, -1);
  expect_file(files, 2, "R", "src/new.c");
  expect_counts(files, 2, 2, 3);
  expect_counts(files, 3, -1, -1);
  g_assert_cmpuint(files->len, ==, 4);   /* src/gone.c added nothing */
  g_assert_cmpint(s.changed, ==, 4);     /* numstat never changes the count */
  g_ptr_array_unref(files);
}

/* No numstat at all (unborn HEAD: `git diff HEAD` fails, or the caller never
 * asked): every file keeps its -1s. */
static void test_numstat_absent(void) {
  PtGitStatus s;
  GPtrArray *files = parse("? a.c\n", NULL, &s);
  expect_counts(files, 0, -1, -1);
  g_ptr_array_unref(files);
}

/* Empty numstat text merges nothing and breaks nothing. */
static void test_numstat_empty(void) {
  PtGitStatus s;
  GPtrArray *files = parse(
      "1 .M N... 100644 100644 100644 aaa bbb a.c\n", "", &s);
  expect_counts(files, 0, -1, -1);
  g_ptr_array_unref(files);
}

/* ---------- the branch chip ----------
 *
 * One spelling for every surface that shows a project's git state (sidebar row,
 * project bar chip); the tests pin the text, because two surfaces drifting
 * apart is exactly what the shared formatter exists to prevent. */

static char *chip(const PtGitStatus *st) {
  static char buf[192];
  g_strlcpy(buf, "poison", sizeof buf);   /* every path must overwrite this */
  pt_git_format_chip(st, buf, sizeof buf);
  return buf;
}

/* A clean tree is the branch and nothing else. */
static void test_chip_clean(void) {
  PtGitStatus s = { 0 };
  g_strlcpy(s.branch, "main", sizeof s.branch);
  g_assert_cmpstr(chip(&s), ==, "main");
}

/* Dirty adds the count behind a ✚, one space out. */
static void test_chip_changed(void) {
  PtGitStatus s = { .changed = 12 };
  g_strlcpy(s.branch, "feature/x", sizeof s.branch);
  g_assert_cmpstr(chip(&s), ==, "feature/x ✚12");
}

/* No branch means no chip, even with changed files: a bare " ✚3" names nothing.
 * Covers a non-repo, and a repo polled before its HEAD was read. */
static void test_chip_no_branch(void) {
  PtGitStatus s = { .changed = 3 };
  g_assert_cmpstr(chip(&s), ==, "");
  g_assert_cmpstr(chip(NULL), ==, "");
}

/* The status comes straight from a parse in real callers; ahead/behind are the
 * status bar's business and never reach the chip. */
static void test_chip_from_parse(void) {
  PtGitStatus s;
  GPtrArray *files = parse(
      "# branch.head main\n"
      "# branch.ab +4 -9\n"
      "? a.c\n", NULL, &s);
  g_assert_cmpstr(chip(&s), ==, "main ✚1");
  g_ptr_array_unref(files);
}

/* A buffer too small truncates and stays terminated. */
static void test_chip_truncates(void) {
  PtGitStatus s = { .changed = 2 };
  g_strlcpy(s.branch, "long-branch-name", sizeof s.branch);
  char buf[5];
  pt_git_format_chip(&s, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "long");
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/gitparse/clean", test_clean_repo);
  g_test_add_func("/gitparse/dirty", test_dirty_repo);
  g_test_add_func("/gitparse/detached", test_detached_head);
  g_test_add_func("/gitparse/null", test_null_input);
  g_test_add_func("/gitparse/files/kinds", test_files_kinds);
  g_test_add_func("/gitparse/files/renames", test_files_renames);
  g_test_add_func("/gitparse/files/spaces", test_files_path_with_spaces);
  g_test_add_func("/gitparse/files/empty", test_files_empty);
  g_test_add_func("/gitparse/numstat/counts", test_numstat_counts);
  g_test_add_func("/gitparse/numstat/binary", test_numstat_binary);
  g_test_add_func("/gitparse/numstat/renames", test_numstat_renames);
  g_test_add_func("/gitparse/numstat/merge", test_numstat_merge);
  g_test_add_func("/gitparse/numstat/absent", test_numstat_absent);
  g_test_add_func("/gitparse/numstat/empty", test_numstat_empty);
  g_test_add_func("/gitparse/chip/clean", test_chip_clean);
  g_test_add_func("/gitparse/chip/changed", test_chip_changed);
  g_test_add_func("/gitparse/chip/no-branch", test_chip_no_branch);
  g_test_add_func("/gitparse/chip/from-parse", test_chip_from_parse);
  g_test_add_func("/gitparse/chip/truncates", test_chip_truncates);
  return g_test_run();
}
