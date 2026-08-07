#include "pt-codex-usage.h"
#include "pt-json-read.h"
#include <glib/gstdio.h>
#include <string.h>

/* The bounds that keep a timer-driven read off an ever-growing directory from
 * turning into a directory walk. A session log that has not been touched in
 * three days does not describe the account now, and the newest handful of
 * files is where the current session always is. */
#define PT_CODEX_MAX_DAYS  3
#define PT_CODEX_MAX_FILES 12
/* The newest token_count is the last line of the file, so only the tail is
 * worth reading. 64K covers many turns of a chatty session. */
#define PT_CODEX_TAIL_BYTES (64 * 1024)
/* The header line runs to ~22K in current Codex, almost all of it the system
 * prompt. cwd sits in the first few hundred bytes, so this is all that is
 * read — see pt_codex_log_cwd for why it is not parsed as JSON. */
#define PT_CODEX_HEAD_BYTES 4096

char *pt_codex_home(void) {
  const char *env = g_getenv("CODEX_HOME");
  if (env != NULL && env[0] != '\0') return g_strdup(env);
  return g_build_filename(g_get_home_dir(), ".codex", NULL);
}

/* When a window resets, however this version of Codex spelled it. Newer builds
 * write an absolute `resets_at`; older ones wrote the seconds remaining under
 * a different key, which only means anything against the moment it was read. */
static gint64 window_epoch(JsonObject *o, gint64 now) {
  if (o == NULL) return 0;
  gint64 at = pt_json_epoch(o, "resets_at");
  if (at > 0) return at;
  gint64 rel = pt_json_int(o, "resets_in_seconds", -1);
  return rel >= 0 ? now + rel : 0;
}

/* How a window is named on screen, from its length. The two that matter get
 * words because that is how the plans are sold and how people talk about
 * them; everything else falls back to its duration. */
static void window_label(gint64 minutes, char *buf, gsize len) {
  if (minutes == 10080)     g_strlcpy(buf, "weekly", len);
  else if (minutes == 1440) g_strlcpy(buf, "daily", len);
  else if (minutes <= 0)    g_strlcpy(buf, "limit", len);
  else if (minutes < 60)    g_snprintf(buf, len, "%" G_GINT64_FORMAT "m limit", minutes);
  else if (minutes % 1440 == 0)
    g_snprintf(buf, len, "%" G_GINT64_FORMAT "d limit", minutes / 1440);
  else
    g_snprintf(buf, len, "%" G_GINT64_FORMAT "h limit", minutes / 60);
}

/* The percentage must actually be a number. Present-but-not-numeric — a build
 * that starts writing "33.0" as a string — would otherwise read as 0.0 and
 * draw a bar saying the window is untouched, which is the one way this panel
 * must not be wrong. No window is better than a reassuring one. */
static void add_codex_window(PtUsage *u, JsonObject *w, gint64 now) {
  double pct;
  if (w == NULL || !pt_json_number(w, "used_percent", &pct)) return;
  char label[64];
  window_label(pt_json_int(w, "window_minutes", 0), label, sizeof(label));
  pt_usage_add_window(u, label, pct, window_epoch(w, now));
}

/* ---------- parsing ---------- */
/* One line: TRUE and `out` filled when it is a token_count event carrying
 * limits. Everything else — other event types, half a line from the tail cut,
 * a version that renamed the event — is not an error, it is just not this. */
static gboolean parse_line(const char *line, gsize len, gint64 now,
                           gboolean want_context, PtUsage *out) {
  if (len == 0 || line[0] != '{') return FALSE;
  /* Cheap sieve before the parse. Most of a rollout log is transcript, and
   * some of those lines are tens of kilobytes; the event type has to appear
   * literally, so a line without it cannot be the one being looked for. */
  if (g_strstr_len(line, (gssize)len, "\"token_count\"") == NULL) return FALSE;
  JsonParser *p = json_parser_new();
  gboolean ok = json_parser_load_from_data(p, line, (gssize)len, NULL);
  JsonNode *root = ok ? json_parser_get_root(p) : NULL;
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(p);
    return FALSE;
  }
  JsonObject *payload = pt_json_obj(json_node_get_object(root), "payload");
  if (payload == NULL ||
      g_strcmp0(pt_json_string(payload, "type"), "token_count") != 0) {
    g_object_unref(p);
    return FALSE;
  }

  PtUsage u;
  pt_usage_clear(&u);
  u.kind = PT_AGENT_CODEX;
  g_strlcpy(u.source, "codex session log", sizeof(u.source));

  JsonObject *limits = pt_json_obj(payload, "rate_limits");
  if (limits != NULL) {
    const char *plan = pt_json_string(limits, "plan_type");
    if (plan != NULL) g_strlcpy(u.plan, plan, sizeof(u.plan));
    /* Not a percentage that happens to be high: Codex says outright when a
     * limit was actually hit, and that is worth more than any bar. */
    u.limit_hit = pt_json_string(limits, "rate_limit_reached_type") != NULL;
    add_codex_window(&u, pt_json_obj(limits, "primary"), now);
    add_codex_window(&u, pt_json_obj(limits, "secondary"), now);
  }

  JsonObject *info = pt_json_obj(payload, "info");
  if (want_context && info != NULL) {
    /* last_token_usage, never total_token_usage: the total sums every turn of
     * the session and runs into the millions, while the context window only
     * ever holds what the newest request carried. */
    JsonObject *last = pt_json_obj(info, "last_token_usage");
    u.ctx_used = last != NULL ? pt_json_int(last, "total_tokens", 0) : 0;
    u.ctx_limit = pt_json_int(info, "model_context_window", 0);
  }

  g_object_unref(p);
  /* A token_count with neither a window nor a context reading has nothing to
   * show, and accepting it would stop the scan short of a line that has. */
  if (u.n_windows == 0 && pt_usage_context_percent(&u) < 0) return FALSE;
  *out = u;
  return TRUE;
}

gboolean pt_codex_usage_parse(const char *jsonl, gint64 now,
                              gboolean want_context, PtUsage *out) {
  if (jsonl == NULL || out == NULL) return FALSE;
  /* Backwards: the newest token_count is the one that describes the account
   * now, and it is near the end. Scanning from the back means a long log costs
   * one parse rather than one per turn. */
  gboolean found = FALSE;
  const char *end = jsonl + strlen(jsonl);
  while (end > jsonl && !found) {
    const char *start = end;
    while (start > jsonl && start[-1] != '\n') start--;
    gsize len = (gsize)(end - start);
    while (len > 0 && (start[len - 1] == '\r' || start[len - 1] == '\n')) len--;
    if (parse_line(start, len, now, want_context, out)) found = TRUE;
    end = start > jsonl ? start - 1 : jsonl;
    if (start == jsonl) break;
  }
  return found;
}

/* Deliberately not a JSON parse of the whole line.
 *
 * The header carries Codex's entire system prompt inline — some 22K — and this
 * runs once per candidate file on a timer, for one string that sits in the
 * first few hundred bytes. Parsing all of it to reach that string cost more
 * than a frame's worth of time per poll, spent almost entirely on text nobody
 * here reads.
 *
 * So the key is found by hand and only its value is handed to a real parser,
 * which is what undoes the escapes. `"cwd":"` cannot appear inside a JSON
 * string — a quote in one is written \" — so the only way to find it is as an
 * actual member name; the first such member wins, which is Codex's own. */
char *pt_codex_log_cwd(const char *header) {
  if (header == NULL || header[0] != '{') return NULL;
  const char *nl = strchr(header, '\n');
  gssize limit = nl != NULL ? (gssize)(nl - header) : (gssize)strlen(header);
  static const char key[] = "\"cwd\":\"";
  const char *at = g_strstr_len(header, limit, key);
  if (at == NULL) return NULL;
  const char *v = at + sizeof(key) - 1;
  const char *stop = header + limit;
  const char *end = v;
  while (end < stop && *end != '"') end += (*end == '\\' && end + 1 < stop) ? 2 : 1;
  if (end >= stop) return NULL;   /* the value ran past what was read */

  char *doc = g_strdup_printf("{\"cwd\":\"%.*s\"}", (int)(end - v), v);
  JsonParser *p = json_parser_new();
  char *cwd = NULL;
  if (json_parser_load_from_data(p, doc, -1, NULL)) {
    JsonNode *root = json_parser_get_root(p);
    if (root != NULL && JSON_NODE_HOLDS_OBJECT(root)) {
      const char *s = pt_json_string(json_node_get_object(root), "cwd");
      if (s != NULL) cwd = g_strdup(s);
    }
  }
  g_object_unref(p);
  g_free(doc);
  return cwd;
}

/* ---------- finding the log ---------- */
typedef struct { char *path; gint64 mtime; } Candidate;

static void candidate_free(gpointer p) {
  Candidate *c = p;
  g_free(c->path);
  g_free(c);
}

static int candidate_newest_first(gconstpointer a, gconstpointer b) {
  const Candidate *x = *(Candidate *const *)a;
  const Candidate *y = *(Candidate *const *)b;
  if (x->mtime == y->mtime) return 0;
  return x->mtime > y->mtime ? -1 : 1;
}

/* Directory entry names, sorted newest-name-first. The tree is YYYY/MM/DD, so
 * the name order is the date order at every level. */
static GPtrArray *sorted_entries(const char *dir) {
  GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
  GDir *d = g_dir_open(dir, 0, NULL);
  if (d == NULL) return names;
  const char *name;
  while ((name = g_dir_read_name(d)) != NULL)
    g_ptr_array_add(names, g_strdup(name));
  g_dir_close(d);
  g_ptr_array_sort_values(names, (GCompareFunc)g_strcmp0);
  /* Sorted ascending; reverse in place for newest first. */
  for (guint i = 0, j = names->len; i + 1 < j; i++, j--) {
    gpointer tmp = g_ptr_array_index(names, i);
    g_ptr_array_index(names, i) = g_ptr_array_index(names, j - 1);
    g_ptr_array_index(names, j - 1) = tmp;
  }
  return names;
}

/* The rollout files in the newest PT_CODEX_MAX_DAYS day directories, newest
 * modification first, capped at PT_CODEX_MAX_FILES. */
static GPtrArray *find_candidates(const char *sessions_dir) {
  GPtrArray *out = g_ptr_array_new_with_free_func(candidate_free);
  int days = 0;
  GPtrArray *years = sorted_entries(sessions_dir);
  for (guint y = 0; y < years->len && days < PT_CODEX_MAX_DAYS; y++) {
    char *ydir = g_build_filename(sessions_dir,
                                  g_ptr_array_index(years, y), NULL);
    GPtrArray *months = sorted_entries(ydir);
    for (guint m = 0; m < months->len && days < PT_CODEX_MAX_DAYS; m++) {
      char *mdir = g_build_filename(ydir, g_ptr_array_index(months, m), NULL);
      GPtrArray *dd = sorted_entries(mdir);
      for (guint i = 0; i < dd->len && days < PT_CODEX_MAX_DAYS; i++) {
        char *ddir = g_build_filename(mdir, g_ptr_array_index(dd, i), NULL);
        GDir *d = g_dir_open(ddir, 0, NULL);
        if (d != NULL) {
          days++;
          const char *name;
          while ((name = g_dir_read_name(d)) != NULL) {
            if (!g_str_has_prefix(name, "rollout-") ||
                !g_str_has_suffix(name, ".jsonl"))
              continue;
            char *path = g_build_filename(ddir, name, NULL);
            GStatBuf st;
            if (g_stat(path, &st) == 0) {
              Candidate *c = g_new0(Candidate, 1);
              c->path = path;
              c->mtime = (gint64)st.st_mtime;
              g_ptr_array_add(out, c);
            } else {
              g_free(path);
            }
          }
          g_dir_close(d);
        }
        g_free(ddir);
      }
      g_ptr_array_free(dd, TRUE);
      g_free(mdir);
    }
    g_ptr_array_free(months, TRUE);
    g_free(ydir);
  }
  g_ptr_array_free(years, TRUE);

  g_ptr_array_sort(out, candidate_newest_first);
  if (out->len > PT_CODEX_MAX_FILES)
    g_ptr_array_remove_range(out, PT_CODEX_MAX_FILES,
                             out->len - PT_CODEX_MAX_FILES);
  return out;
}

/* The last `cap` bytes of a file, starting at a line boundary so the first
 * line handed to the parser is a whole one. NUL-terminated. Caller frees. */
static char *read_tail(const char *path, gsize cap) {
  GFile *f = g_file_new_for_path(path);
  GFileInputStream *in = g_file_read(f, NULL, NULL);
  g_object_unref(f);
  if (in == NULL) return NULL;

  GFileInfo *fi = g_file_input_stream_query_info(
      in, G_FILE_ATTRIBUTE_STANDARD_SIZE, NULL, NULL);
  goffset size = fi != NULL ? g_file_info_get_size(fi) : 0;
  g_clear_object(&fi);
  goffset from = size > (goffset)cap ? size - (goffset)cap : 0;
  if (from > 0 &&
      !g_seekable_seek(G_SEEKABLE(in), from, G_SEEK_SET, NULL, NULL)) {
    g_object_unref(in);
    return NULL;
  }
  gsize want = (gsize)(size - from);
  char *buf = g_malloc(want + 1);
  gsize got = 0;
  if (!g_input_stream_read_all(G_INPUT_STREAM(in), buf, want, &got, NULL,
                               NULL)) {
    g_free(buf);
    g_object_unref(in);
    return NULL;
  }
  buf[got] = '\0';
  g_object_unref(in);
  if (from > 0) {
    /* Started mid-line; drop the fragment. */
    char *nl = strchr(buf, '\n');
    if (nl == NULL) { g_free(buf); return NULL; }
    char *whole = g_strdup(nl + 1);
    g_free(buf);
    return whole;
  }
  return buf;
}

/* The front of the file, NUL-terminated — enough of the header line to find
 * the cwd in, not the whole of it. Caller frees. */
static char *read_head(const char *path) {
  GFile *f = g_file_new_for_path(path);
  GFileInputStream *in = g_file_read(f, NULL, NULL);
  g_object_unref(f);
  if (in == NULL) return NULL;
  char *buf = g_malloc(PT_CODEX_HEAD_BYTES + 1);
  gsize got = 0;
  g_input_stream_read_all(G_INPUT_STREAM(in), buf, PT_CODEX_HEAD_BYTES, &got,
                          NULL, NULL);
  g_object_unref(in);
  buf[got] = '\0';
  return buf;
}

gboolean pt_codex_usage_read(const char *codex_home, const char *cwd,
                             gint64 now, PtUsage *out) {
  if (codex_home == NULL || out == NULL) return FALSE;
  char *sessions = g_build_filename(codex_home, "sessions", NULL);
  GPtrArray *cands = find_candidates(sessions);
  g_free(sessions);

  PtUsage fallback;
  gboolean have_fallback = FALSE;
  gboolean matched = FALSE;
  for (guint i = 0; i < cands->len && !matched; i++) {
    const Candidate *c = g_ptr_array_index(cands, i);
    char *head = read_head(c->path);
    char *log_cwd = pt_codex_log_cwd(head);
    g_free(head);
    gboolean is_ours = cwd != NULL && log_cwd != NULL &&
                       strcmp(cwd, log_cwd) == 0;
    g_free(log_cwd);
    /* Every candidate could hold the account's limits, but reading each one's
     * tail to find that out would defeat the point of the cap. Only the
     * session on screen and the single newest log are worth a tail read: the
     * first gives the context bar, the second is the fallback for limits when
     * no pane's session is in the list. */
    if (!is_ours && have_fallback) continue;
    char *tail = read_tail(c->path, PT_CODEX_TAIL_BYTES);
    if (tail == NULL) continue;
    PtUsage u;
    if (pt_codex_usage_parse(tail, now, is_ours, &u)) {
      if (is_ours) { *out = u; matched = TRUE; }
      else if (!have_fallback) { fallback = u; have_fallback = TRUE; }
    }
    g_free(tail);
  }
  g_ptr_array_free(cands, TRUE);

  if (matched) return TRUE;
  if (have_fallback) { *out = fallback; return TRUE; }
  return FALSE;
}
