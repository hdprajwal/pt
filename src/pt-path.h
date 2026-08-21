#pragma once
#include <glib.h>

/* "/home/me/dev/foo" → "~/dev/foo", written into `buf`, which always ends up
 * NUL-terminated and truncates like g_strlcpy when the result does not fit.
 * Only a whole leading path component matches, so "/home/metoo" is left alone;
 * a trailing slash on `home` is tolerated. A NULL path writes ""; a NULL or
 * empty `home` copies the path through untouched, and so does a `home` of "/",
 * whose one component no path can follow with a slash.
 * `home` is a parameter rather than a g_get_home_dir() call inside so the rule
 * is testable; callers pass g_get_home_dir(). Shared so every surface that
 * shows a project path spells it the same way. */
void pt_path_home_abbrev(const char *path, const char *home,
                         char *buf, gsize cap);

/* `path` with every trailing slash stripped — the root "/" stays "/" — so the
 * two sides of a project-path comparison can be run through here and "/a/pt"
 * matches "/a/pt/". NULL gives NULL; the result is newly allocated. */
char *pt_path_normalize(const char *path);
