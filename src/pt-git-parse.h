#pragma once
#include <glib.h>

typedef struct {
  char branch[128];   /* "" when unknown; "(detached)" for detached HEAD */
  int ahead;
  int behind;
  int changed;        /* changed + untracked + unmerged entry count */
} PtGitStatus;

/* Parse `git status --porcelain=v2 --branch` output.
 * Returns FALSE only when text is NULL. Unknown lines are ignored. */
gboolean pt_git_parse_porcelain_v2(const char *text, PtGitStatus *out);
