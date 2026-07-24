#include "pt-git-parse.h"
#include <stdio.h>
#include <string.h>

gboolean pt_git_parse_porcelain_v2(const char *text, PtGitStatus *out) {
  if (text == NULL || out == NULL) return FALSE;
  memset(out, 0, sizeof(*out));

  char **lines = g_strsplit(text, "\n", -1);
  for (char **lp = lines; *lp != NULL; lp++) {
    const char *line = *lp;
    if (g_str_has_prefix(line, "# branch.head ")) {
      g_strlcpy(out->branch, line + strlen("# branch.head "),
                sizeof(out->branch));
    } else if (g_str_has_prefix(line, "# branch.ab ")) {
      sscanf(line, "# branch.ab +%d -%d", &out->ahead, &out->behind);
    } else if (line[0] == '1' || line[0] == '2' || line[0] == 'u' ||
               line[0] == '?') {
      /* Entry lines all start with a type tag followed by a space. */
      if (line[1] == ' ') out->changed++;
    }
  }
  g_strfreev(lines);
  return TRUE;
}
