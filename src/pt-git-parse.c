#include "pt-git-parse.h"
#include <stdio.h>
#include <string.h>

static void git_file_free(gpointer data) {
  PtGitFile *f = data;
  g_free(f->path);
  g_free(f);
}

/* Advance past `n` space-terminated fields inside [p, end). NULL when the
 * line runs out of columns first. */
static const char *skip_fields(const char *p, const char *end, int n) {
  for (int i = 0; i < n; i++) {
    const char *sp = memchr(p, ' ', (size_t)(end - p));
    if (sp == NULL) return NULL;
    p = sp + 1;
  }
  return p < end ? p : NULL;
}

/* One porcelain entry line ("1 ", "2 ", "u ", "? ") → one PtGitFile. The
 * fixed columns are skipped with a pointer scan; everything after them is the
 * path, verbatim — a path may contain spaces. */
static void parse_entry(const char *line, const char *end, GPtrArray *files) {
  int skip;
  switch (line[0]) {
    case '1': skip = 8; break;
    case '2': skip = 9; break;
    case 'u': skip = 10; break;
    case '?': skip = 1; break;
    default: return;    /* unknown entry kinds are ignored */
  }
  const char *path = skip_fields(line, end, skip);
  if (path == NULL) return;
  /* Rename entries carry "<new>\t<orig>"; the new path is the one to show. */
  const char *stop = memchr(path, '\t', (size_t)(end - path));
  if (stop == NULL) stop = end;

  PtGitFile *f = g_new0(PtGitFile, 1);
  f->add = f->del = -1;   /* until a numstat row says otherwise */
  if (line[0] == '?') {
    g_strlcpy(f->xy, "??", sizeof(f->xy));
  } else {
    int n = 0;
    for (const char *c = line + 2; c < line + 4 && c < end && *c != ' '; c++)
      if (*c != '.') f->xy[n++] = *c;
    f->xy[n] = '\0';
  }
  f->path = g_strndup(path, (gsize)(stop - path));
  g_ptr_array_add(files, f);
}

/* Numstat renames arrive with the unchanged parts factored out:
 * "old.c => new.c", "src/{old => new}.c", "dir/{ => sub}/f.c". Rebuild the new
 * path by keeping the right-hand side of every brace group (or of a bare
 * arrow). */
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

/* Walk `git diff --numstat` rows ("add\tdel\tpath") and copy the counts onto
 * the files with matching paths. Binary rows ("-\t-") keep the file at -1. */
static void merge_numstat(GPtrArray *files, const char *text) {
  if (text == NULL || files->len == 0) return;
  GHashTable *by_path = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; i < files->len; i++) {
    PtGitFile *f = g_ptr_array_index(files, i);
    g_hash_table_insert(by_path, f->path, f);
  }
  for (const char *line = text; *line != '\0';) {
    const char *nl = strchr(line, '\n');
    const char *end = nl != NULL ? nl : line + strlen(line);
    const char *tab1 = memchr(line, '\t', (size_t)(end - line));
    const char *tab2 = tab1 != NULL
        ? memchr(tab1 + 1, '\t', (size_t)(end - tab1 - 1)) : NULL;
    if (tab1 != NULL && tab1 > line && tab2 != NULL && tab2 + 1 < end) {
      char *spec = g_strndup(tab2 + 1, (gsize)(end - tab2 - 1));
      char *path = numstat_new_path(spec);
      PtGitFile *f = g_hash_table_lookup(by_path, path);
      if (f != NULL) {
        /* Binary files report "-" in both columns and have no line counts. */
        gboolean binary = line[0] == '-' || tab1[1] == '-';
        f->add = binary ? -1 : (int)g_ascii_strtoll(line, NULL, 10);
        f->del = binary ? -1 : (int)g_ascii_strtoll(tab1 + 1, NULL, 10);
      }
      g_free(path);
      g_free(spec);
    }
    line = nl != NULL ? nl + 1 : end;
  }
  g_hash_table_destroy(by_path);
}

void pt_git_result_parse(const char *status_text,
                         const char *numstat_text_or_null,
                         PtGitStatus *out_status, GPtrArray **out_files) {
  memset(out_status, 0, sizeof(*out_status));
  GPtrArray *files = g_ptr_array_new_with_free_func(git_file_free);

  /* One walk, no buffer split: each iteration sees [line, end). Prefix checks
   * are safe on the running pointer — no header prefix spans a newline. */
  for (const char *line = status_text; line != NULL && *line != '\0';) {
    const char *nl = strchr(line, '\n');
    const char *end = nl != NULL ? nl : line + strlen(line);
    if (g_str_has_prefix(line, "# branch.head ")) {
      const char *head = line + strlen("# branch.head ");
      gsize n = MIN((gsize)(end - head), sizeof(out_status->branch) - 1);
      memcpy(out_status->branch, head, n);
      out_status->branch[n] = '\0';
    } else if (g_str_has_prefix(line, "# branch.ab ")) {
      /* %d stops at the newline on its own; the format never crosses it. */
      sscanf(line, "# branch.ab +%d -%d", &out_status->ahead,
             &out_status->behind);
    } else if (end - line >= 2 && line[1] == ' ') {
      parse_entry(line, end, files);
    }
    line = nl != NULL ? nl + 1 : end;
  }

  /* Structural, not counted: the entry lines *are* the changed set. */
  out_status->changed = (int)files->len;
  merge_numstat(files, numstat_text_or_null);
  *out_files = files;
}

void pt_git_format_chip(const PtGitStatus *st, char *buf, gsize cap) {
  if (buf == NULL || cap == 0) return;
  buf[0] = '\0';
  if (st == NULL || st->branch[0] == '\0') return;
  if (st->changed > 0) g_snprintf(buf, cap, "%s ✚%d", st->branch, st->changed);
  else                 g_strlcpy(buf, st->branch, cap);
}
