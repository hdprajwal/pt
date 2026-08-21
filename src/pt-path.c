#include "pt-path.h"

#include <string.h>   /* strncmp */

void pt_path_home_abbrev(const char *path, const char *home,
                         char *buf, gsize cap) {
  if (buf == NULL || cap == 0) return;
  buf[0] = '\0';
  if (path == NULL) return;
  if (home == NULL || home[0] == '\0') { g_strlcpy(buf, path, cap); return; }

  gsize n = strlen(home);
  while (n > 1 && home[n - 1] == '/') n--;   /* tolerate a trailing slash */
  if (strncmp(path, home, n) != 0) { g_strlcpy(buf, path, cap); return; }
  if (path[n] == '\0') { g_strlcpy(buf, "~", cap); return; }
  /* A component boundary, or the match was only part of a name. */
  if (path[n] != '/') { g_strlcpy(buf, path, cap); return; }

  if (cap < 2) return;   /* room for "~" plus its terminator, or nothing */
  buf[0] = '~';
  g_strlcpy(buf + 1, path + n, cap - 1);
}

char *pt_path_normalize(const char *path) {
  if (path == NULL) return NULL;
  char *out = g_strdup(path);
  gsize n = strlen(out);
  while (n > 1 && out[n - 1] == '/') out[--n] = '\0';
  return out;
}
