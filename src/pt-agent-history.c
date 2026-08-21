#include "pt-agent-history.h"
#include "pt-agent-session.h"
#include "pt-json-read.h"
#include <json-glib/json-glib.h>
#include <string.h>

void pt_agent_history_entry_free(PtAgentHistoryEntry *e) {
  if (e == NULL) return;
  g_free(e->session_id);
  g_free(e->cwd);
  g_clear_pointer(&e->ts, g_date_time_unref);
  g_free(e);
}

/* The one field report_load does not carry back. A second parse per file,
 * rather than a hand-rolled reader beside the shared gate: every rule
 * report_load enforces (version, agent name, session-id charset, pid) then
 * lives in exactly one place, and this pass only ever touches files it has
 * already accepted. */
static GDateTime *read_ts(const char *path) {
  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_file(parser, path, NULL)) {
    g_object_unref(parser);
    return NULL;
  }
  JsonNode *root = json_parser_get_root(parser);
  GDateTime *ts = NULL;
  if (root != NULL && JSON_NODE_HOLDS_OBJECT(root)) {
    const char *s = pt_json_string(json_node_get_object(root), "ts");
    /* NULL default zone: only an embedded offset counts, and the writer
     * always puts one in (format_iso8601 of a UTC time ends in Z). */
    if (s != NULL) ts = g_date_time_new_from_iso8601(s, NULL);
  }
  g_object_unref(parser);
  return ts;
}

static int cmp_newest_first(gconstpointer a, gconstpointer b) {
  const PtAgentHistoryEntry *ea = *(PtAgentHistoryEntry *const *)a;
  const PtAgentHistoryEntry *eb = *(PtAgentHistoryEntry *const *)b;
  if (ea->ts != NULL && eb->ts != NULL) {
    gint64 diff = g_date_time_difference(eb->ts, ea->ts);
    if (diff != 0) return diff > 0 ? 1 : -1;   /* newer sorts first */
  } else if (ea->ts != NULL) {
    return -1;   /* a readable stamp beats an unreadable one */
  } else if (eb->ts != NULL) {
    return 1;
  }
  /* Equal stamps — two agents started in the same second, or both unreadable
   * — tie-break on the id so two loads answer with the same order. */
  return g_strcmp0(ea->session_id, eb->session_id);
}

GPtrArray *pt_agent_history_load(const char *dir) {
  GPtrArray *out = g_ptr_array_new_with_free_func(
      (GDestroyNotify)pt_agent_history_entry_free);
  if (dir == NULL) return out;
  GDir *d = g_dir_open(dir, 0, NULL);
  if (d == NULL) return out;
  const char *name;
  while ((name = g_dir_read_name(d)) != NULL) {
    if (!g_str_has_suffix(name, ".json")) continue;
    char *path = g_build_filename(dir, name, NULL);
    PtAgentSessionReport *r = pt_agent_session_report_load(path);
    if (r == NULL) {   /* not pt's, or no longer trustworthy: skip */
      g_free(path);
      continue;
    }
    PtAgentHistoryEntry *e = g_new0(PtAgentHistoryEntry, 1);
    e->agent = r->agent;
    e->session_id = g_strdup(r->session_id);
    e->cwd = g_strdup(r->cwd);
    e->ts = read_ts(path);
    pt_agent_session_report_free(r);
    g_free(path);
    g_ptr_array_add(out, e);
  }
  g_dir_close(d);
  g_ptr_array_sort(out, cmp_newest_first);
  return out;
}

char *pt_agent_history_relative_time(GDateTime *ts, GDateTime *now) {
  if (ts == NULL || now == NULL) return g_strdup("");
  gint64 secs = g_date_time_difference(now, ts) / G_USEC_PER_SEC;
  /* A stamp ahead of `now` is clock skew between the machine that wrote the
   * report and this one; "now" is closer to the truth than a negative age. */
  if (secs < 0) secs = 0;
  if (secs < 60) return g_strdup("now");
  if (secs < 3600)
    return g_strdup_printf("%" G_GINT64_FORMAT "m ago", secs / 60);
  if (secs < 86400)
    return g_strdup_printf("%" G_GINT64_FORMAT "h ago", secs / 3600);
  return g_strdup_printf("%" G_GINT64_FORMAT "d ago", secs / 86400);
}
