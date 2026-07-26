#pragma once
#include <glib.h>

typedef struct {
  char branch[128];   /* "" when unknown; "(detached)" for detached HEAD */
  int ahead;
  int behind;
  int changed;        /* changed + untracked + unmerged entry count */
} PtGitStatus;

/* One changed path. `xy` is the display code: "??" for untracked, otherwise the
 * porcelain X/Y letters with '.' dropped — "M", "MM", "UU", i.e. what
 * `git status -s` prints, minus the rename arrow. */
typedef struct {
  char xy[4];
  char *path;
  int add, del;   /* lines; -1 = unknown (binary, untracked, no counts yet) */
} PtGitFile;

/* One `git diff --numstat` row. add/del are -1 for binary files ("-  -"). */
typedef struct {
  char *path;
  int add, del;
} PtGitNumstat;

/* Parse `git status --porcelain=v2 --branch` output.
 * Returns FALSE only when text is NULL. Unknown lines are ignored. */
gboolean pt_git_parse_porcelain_v2(const char *text, PtGitStatus *out);

/* Changed paths from the same output, in git's order. Elements are PtGitFile*
 * and the array frees them. Never NULL: NULL text or no entries gives an empty
 * array. Renames report the new path. */
GPtrArray *pt_git_parse_files(const char *text);

/* Deep copy of such an array; NULL gives an empty one. */
GPtrArray *pt_git_files_copy(GPtrArray *files);

/* Parse `git diff --numstat` output. Elements are PtGitNumstat*; the array
 * frees them. Never NULL. Rename rows ("a => b", "src/{a => b}.c") resolve to
 * the new path. */
GPtrArray *pt_git_parse_numstat(const char *text);

/* Copy counts onto the files with matching paths. Rows that match nothing are
 * dropped, and files with no row keep their -1s. */
void pt_git_files_merge_numstat(GPtrArray *files, GPtrArray *stats);
