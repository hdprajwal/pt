#pragma once
#include <glib.h>

typedef struct {
  char branch[128];   /* "" when unknown; "(detached)" for detached HEAD */
  int ahead;
  int behind;
  int changed;        /* == the length of the file list parsed with it */
} PtGitStatus;

/* One changed path. `xy` is the display code: "??" for untracked, otherwise the
 * porcelain X/Y letters with '.' dropped — "M", "MM", "UU", i.e. what
 * `git status -s` prints, minus the rename arrow. */
typedef struct {
  char xy[4];
  char *path;
  int add, del;   /* lines; -1 = unknown (binary, untracked, no counts yet) */
} PtGitFile;

/* One pass over `git status --porcelain=v2 --branch` output: branch and
 * ahead/behind from the headers, one PtGitFile per entry line in git's order
 * (renames report the new path), and `out_status->changed` set from the file
 * count — the two can never disagree. When `numstat_text_or_null` carries
 * `git diff --numstat` output, its line counts land on the matching paths in
 * the same call: files with no row keep their -1s (untracked, binary), rows
 * matching no file are dropped, and rename rows ("a => b", "src/{a => b}.c")
 * resolve to the new path first. NULL status text (not a repository) gives a
 * zeroed status. `*out_files` is never NULL and transfers to the caller; the
 * array frees its PtGitFile* elements. */
void pt_git_result_parse(const char *status_text,
                         const char *numstat_text_or_null,
                         PtGitStatus *out_status, GPtrArray **out_files);

/* The short git line every surface shows for a project — "main", or
 * "main ✚3" when files changed. `buf` always ends up NUL-terminated, and is
 * empty when there is nothing to show: a NULL status, or one with no branch
 * (not a repository, or polled before HEAD was read) — a bare count names
 * nothing, so it is not printed alone. Lives here rather than in a widget
 * because the sidebar and the project bar must not drift apart. */
void pt_git_format_chip(const PtGitStatus *st, char *buf, gsize cap);
