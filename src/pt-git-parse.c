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

static void git_file_free(gpointer data) {
  PtGitFile *f = data;
  g_free(f->path);
  g_free(f);
}

/* Entry lines are `n_fields` space-separated columns whose last one is the
 * path — and a path may itself contain spaces, so the split has to stop
 * counting at the path and take the rest verbatim. NULL when the line is
 * short of columns. */
static char *entry_path(const char *line, int n_fields) {
  char **tok = g_strsplit(line, " ", n_fields);
  char *path = NULL;
  if (g_strv_length(tok) == (guint)n_fields && tok[n_fields - 1][0] != '\0')
    path = g_strdup(tok[n_fields - 1]);
  g_strfreev(tok);
  return path;
}

GPtrArray *pt_git_parse_files(const char *text) {
  GPtrArray *files = g_ptr_array_new_with_free_func(git_file_free);
  if (text == NULL) return files;

  char **lines = g_strsplit(text, "\n", -1);
  for (char **lp = lines; *lp != NULL; lp++) {
    const char *line = *lp;
    if (line[0] == '\0' || line[1] != ' ') continue;
    char *path = NULL;
    switch (line[0]) {
      case '1': path = entry_path(line, 9);  break;
      case '2': path = entry_path(line, 10); break;
      case 'u': path = entry_path(line, 11); break;
      case '?': path = entry_path(line, 2);  break;
      default:  continue;   /* "# branch.*" headers and anything unknown */
    }
    if (path == NULL) continue;
    /* Rename entries carry "<new>\t<orig>"; the new path is the one to show. */
    char *tab = strchr(path, '\t');
    if (tab != NULL) *tab = '\0';

    PtGitFile *f = g_new0(PtGitFile, 1);
    f->add = f->del = -1;   /* until a numstat row says otherwise */
    if (line[0] == '?') {
      g_strlcpy(f->xy, "??", sizeof(f->xy));
    } else {
      int n = 0;
      for (int i = 2; i < 4 && line[i] != '\0' && line[i] != ' '; i++)
        if (line[i] != '.') f->xy[n++] = line[i];
      f->xy[n] = '\0';
    }
    f->path = path;
    g_ptr_array_add(files, f);
  }
  g_strfreev(lines);
  return files;
}

GPtrArray *pt_git_files_copy(GPtrArray *files) {
  GPtrArray *copy = g_ptr_array_new_with_free_func(git_file_free);
  for (guint i = 0; files != NULL && i < files->len; i++) {
    const PtGitFile *src = g_ptr_array_index(files, i);
    PtGitFile *f = g_new0(PtGitFile, 1);
    memcpy(f->xy, src->xy, sizeof(f->xy));
    f->path = g_strdup(src->path);
    f->add = src->add;
    f->del = src->del;
    g_ptr_array_add(copy, f);
  }
  return copy;
}

/* ---------- numstat ---------- */
static void numstat_free(gpointer data) {
  PtGitNumstat *s = data;
  g_free(s->path);
  g_free(s);
}

/* Renames arrive with the unchanged parts factored out: "old.c => new.c",
 * "src/{old => new}.c", "dir/{ => sub}/f.c". Rebuild the new path by keeping
 * the right-hand side of every brace group (or of a bare arrow). */
static char *numstat_new_path(const char *spec) {
  static const char arrow[] = " => ";
  if (strstr(spec, arrow) == NULL) return g_strdup(spec);
  if (strchr(spec, '{') == NULL)
    return g_strdup(strstr(spec, arrow) + strlen(arrow));

  GString *out = g_string_new(NULL);
  const char *p = spec, *open;
  while ((open = strchr(p, '{')) != NULL) {
    const char *close = strchr(open, '}');
    const char *arr = close != NULL
        ? g_strstr_len(open, close - open, arrow) : NULL;
    if (arr == NULL) break;
    g_string_append_len(out, p, open - p);
    g_string_append_len(out, arr + strlen(arrow),
                        close - (arr + strlen(arrow)));
    p = close + 1;
  }
  g_string_append(out, p);
  char *path = g_string_free(out, FALSE);
  /* An empty side ("dir/{sub => }/f.c") leaves an empty segment behind. */
  if (strstr(path, "//") != NULL) {
    char **parts = g_strsplit(path, "//", -1);
    char *joined = g_strjoinv("/", parts);
    g_strfreev(parts);
    g_free(path);
    path = joined;
  }
  return path;
}

GPtrArray *pt_git_parse_numstat(const char *text) {
  GPtrArray *stats = g_ptr_array_new_with_free_func(numstat_free);
  if (text == NULL) return stats;

  char **lines = g_strsplit(text, "\n", -1);
  for (char **lp = lines; *lp != NULL; lp++) {
    char **col = g_strsplit(*lp, "\t", 3);
    if (g_strv_length(col) == 3 && col[0][0] != '\0' && col[2][0] != '\0') {
      PtGitNumstat *s = g_new0(PtGitNumstat, 1);
      /* Binary files report "-" in both columns and have no line counts. */
      gboolean binary = col[0][0] == '-' || col[1][0] == '-';
      s->add = binary ? -1 : (int)g_ascii_strtoll(col[0], NULL, 10);
      s->del = binary ? -1 : (int)g_ascii_strtoll(col[1], NULL, 10);
      s->path = numstat_new_path(col[2]);
      g_ptr_array_add(stats, s);
    }
    g_strfreev(col);
  }
  g_strfreev(lines);
  return stats;
}

void pt_git_files_merge_numstat(GPtrArray *files, GPtrArray *stats) {
  if (files == NULL || stats == NULL) return;
  GHashTable *by_path = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; i < stats->len; i++) {
    PtGitNumstat *s = g_ptr_array_index(stats, i);
    g_hash_table_insert(by_path, s->path, s);
  }
  for (guint i = 0; i < files->len; i++) {
    PtGitFile *f = g_ptr_array_index(files, i);
    const PtGitNumstat *s = g_hash_table_lookup(by_path, f->path);
    if (s == NULL) continue;   /* untracked, or a path git no longer lists */
    f->add = s->add;
    f->del = s->del;
  }
  g_hash_table_destroy(by_path);
}
