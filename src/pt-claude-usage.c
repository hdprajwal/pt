#include "pt-claude-usage.h"
#include "pt-json-read.h"
#include <stdlib.h>
#include <string.h>

char *pt_claude_creds_path(void) {
  return g_build_filename(g_get_home_dir(), ".claude", ".credentials.json",
                          NULL);
}

void pt_claude_creds_clear(PtClaudeCreds *c) {
  if (c == NULL) return;
  if (c->token != NULL) {
    /* Wiped, not just freed. The token is the whole of the user's Claude
     * login; it has no business sitting in a reusable heap block after the one
     * request that needed it. */
    memset(c->token, 0, strlen(c->token));
    g_free(c->token);
  }
  memset(c, 0, sizeof(*c));
}

gboolean pt_claude_creds_read(const char *path, PtClaudeCreds *out) {
  if (path == NULL || out == NULL) return FALSE;
  memset(out, 0, sizeof(*out));
  char *text = NULL;
  gsize len = 0;
  if (!g_file_get_contents(path, &text, &len, NULL)) return FALSE;

  JsonParser *p = json_parser_new();
  gboolean ok = json_parser_load_from_data(p, text, (gssize)len, NULL);
  JsonNode *root = ok ? json_parser_get_root(p) : NULL;
  if (root != NULL && JSON_NODE_HOLDS_OBJECT(root)) {
    JsonObject *o = json_node_get_object(root);
    /* The token has lived under a wrapper key for every version that writes
     * this file, but reading the root as a fallback costs one line and covers
     * a flattening. */
    JsonObject *oauth = pt_json_obj(o, "claudeAiOauth");
    if (oauth == NULL) oauth = o;
    const char *tok = pt_json_string(oauth, "accessToken");
    if (tok == NULL) tok = pt_json_string(oauth, "access_token");
    if (tok != NULL && tok[0] != '\0') out->token = g_strdup(tok);
    out->expires_at = pt_json_epoch(oauth, "expiresAt");
    if (out->expires_at == 0) out->expires_at = pt_json_epoch(oauth, "expires_at");
    const char *plan = pt_json_string(oauth, "subscriptionType");
    if (plan == NULL) plan = pt_json_string(oauth, "subscription_type");
    if (plan != NULL) g_strlcpy(out->plan, plan, sizeof(out->plan));
  }
  g_object_unref(p);
  memset(text, 0, len);
  g_free(text);
  return out->token != NULL;
}

/* ---------- transport ---------- */
const char *pt_claude_http_split(const char *text, int *status,
                                 gint64 *retry_after) {
  *status = 0;
  *retry_after = 0;
  if (text == NULL) return "";
  const char *p = text;
  while (g_str_has_prefix(p, "HTTP/")) {
    const char *crlf = strstr(p, "\r\n\r\n");
    const char *lf = strstr(p, "\n\n");
    const char *end;
    gsize skip;
    if (crlf != NULL && (lf == NULL || crlf <= lf)) { end = crlf; skip = 4; }
    else if (lf != NULL)                            { end = lf;   skip = 2; }
    else return p + strlen(p);   /* headers, and nothing after them */

    /* "HTTP/1.1 429 Too Many Requests" and "HTTP/2 429" both put the code
     * after the first space. */
    const char *sp = strchr(p, ' ');
    if (sp != NULL && sp < end) *status = atoi(sp + 1);
    for (const char *line = p; line != NULL && line < end;) {
      if (g_ascii_strncasecmp(line, "retry-after:", 12) == 0) {
        const char *v = line + 12;
        while (*v == ' ' || *v == '\t') v++;
        *retry_after = g_ascii_strtoll(v, NULL, 10);
      }
      const char *nl = memchr(line, '\n', (gsize)(end - line));
      line = nl != NULL ? nl + 1 : NULL;
    }
    p = end + skip;
  }
  return p;
}

/* ---------- response parsing ---------- */
/* The names the endpoint has used for a window's fill, in the order they are
 * tried. The two shapes below disagree with each other in the same document:
 * the keyed windows say `utilization`, the array says `percent`. */
static const char *const percent_keys[] = {
  "utilization", "percent", "used_percent", "percent_used", "usage_percent",
  "percentage",
};

/* Numeric or nothing. A member that is there but has become a string in some
 * later version reads as no percentage at all, not as zero: this is the panel
 * that must never invent room the user does not have.
 *
 * Values in 0..1 are still read as plain percentages. A fraction and a
 * percentage are indistinguishable there — 0.5 is either half or half a
 * percent — and guessing "fraction" would redraw a nearly empty bar as a
 * half-full one, which is the wrong way to be wrong about a limit. */
static gboolean window_percent(JsonObject *w, double *out) {
  for (gsize i = 0; i < G_N_ELEMENTS(percent_keys); i++)
    if (pt_json_number(w, percent_keys[i], out)) return TRUE;
  return FALSE;
}

static gint64 window_resets(JsonObject *w) {
  gint64 at = pt_json_epoch(w, "resets_at");
  if (at == 0) at = pt_json_epoch(w, "reset_at");
  if (at == 0) at = pt_json_epoch(w, "resetsAt");
  return at;
}

/* The model a scoped window applies to, or NULL.
 *
 * The array nests it as scope.model.display_name and leaves `scope` null for a
 * window that covers everything, so a missing model is spelled two levels
 * deep. The flat `model` string is the fallback for a shape that carried it
 * directly. */
static const char *window_model(JsonObject *w) {
  JsonObject *scope = pt_json_obj(w, "scope");
  JsonObject *model = scope != NULL ? pt_json_obj(scope, "model") : NULL;
  const char *name =
      model != NULL ? pt_json_string(model, "display_name") : NULL;
  return name != NULL ? name : pt_json_string(w, "model");
}

/* The window's name as the user knows it. `key` is the member name in the
 * keyed shape or the entry's kind in the array; `model` is which model a
 * scoped cap applies to.
 *
 * "session" and "five_hour" are the same window under two names — the response
 * carries both spellings of it at once, which is what makes the mapping safe
 * to state. */
static void limit_label(const char *key, const char *model, char *buf,
                        gsize len) {
  const char *base = key != NULL ? key : "limit";
  if (g_str_has_prefix(base, "five_hour") || g_str_has_prefix(base, "5h") ||
      g_str_has_prefix(base, "session"))
    base = "5h limit";
  else if (g_str_has_prefix(base, "seven_day") ||
           g_str_has_prefix(base, "weekly"))
    base = "weekly";
  else if (g_str_has_prefix(base, "opus"))
    base = "weekly";

  /* The old shape spelled the per-model cap into the key itself
   * (seven_day_opus), the new one puts it in its own member; either way the
   * model belongs on the label, because two weekly bars with the same name
   * would be unreadable. */
  const char *m = model;
  if (m == NULL && key != NULL) {
    const char *tail = strrchr(key, '_');
    if (tail != NULL && g_ascii_strcasecmp(tail + 1, "opus") == 0)
      m = tail + 1;
  }
  if (m != NULL && m[0] != '\0') g_snprintf(buf, len, "%s · %s", base, m);
  else                           g_strlcpy(buf, base, len);
}

/* The newer shape: a flat `limits` array, one entry per window. */
static gboolean parse_limits_array(JsonArray *limits, PtUsage *u) {
  guint n = json_array_get_length(limits);
  for (guint i = 0; i < n && u->n_windows < PT_USAGE_MAX_WINDOWS; i++) {
    JsonNode *node = json_array_get_element(limits, i);
    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node)) continue;
    JsonObject *w = json_node_get_object(node);
    double pct;
    if (!window_percent(w, &pct)) continue;
    /* `kind` is what the endpoint sends ("session", "weekly_all",
     * "weekly_scoped"); the rest are what other descriptions of it have used.
     * `group` last — it is the coarse bucket, so it only answers when nothing
     * more specific did. */
    const char *key = pt_json_string(w, "kind");
    if (key == NULL) key = pt_json_string(w, "type");
    if (key == NULL) key = pt_json_string(w, "name");
    if (key == NULL) key = pt_json_string(w, "window");
    if (key == NULL) key = pt_json_string(w, "group");
    /* Roomier than the model's label field on purpose: this buffer must not
     * be the thing that truncates, so that pt_usage_add_window is the only
     * place a label is cut and the only place that has to cut it safely. */
    char label[64];
    limit_label(key, window_model(w), label, sizeof(label));
    pt_usage_add_window(u, label, pct, window_resets(w));
  }
  return u->n_windows > 0;
}

/* The older shape: one member per window, named for the window.
 *
 * Membership carries no meaning here the way it does in the array — every
 * object at the root is a candidate, and the response has objects that are not
 * limit windows sitting alongside the ones that are (`spend` reports a
 * `percent` of the month's credit; `extra_usage` a `utilization`). So a
 * window has to prove itself: it has to reset. That is the thing this panel
 * exists to say — used this much, back at this time — and a total that never
 * turns over is not one, whatever it reports a percentage of. */
static gboolean parse_keyed_windows(JsonObject *root, PtUsage *u) {
  GList *members = json_object_get_members(root);
  for (GList *l = members; l != NULL && u->n_windows < PT_USAGE_MAX_WINDOWS;
       l = l->next) {
    const char *key = l->data;
    JsonObject *w = pt_json_obj(root, key);
    if (w == NULL) continue;
    gint64 resets = window_resets(w);
    if (resets == 0) continue;
    double pct;
    if (!window_percent(w, &pct)) continue;
    char label[64];
    limit_label(key, window_model(w), label, sizeof(label));
    pt_usage_add_window(u, label, pct, resets);
  }
  g_list_free(members);
  return u->n_windows > 0;
}

gboolean pt_claude_usage_parse(const char *json, gint64 now, PtUsage *out) {
  (void)now;
  if (json == NULL || out == NULL) return FALSE;
  JsonParser *p = json_parser_new();
  gboolean ok = json_parser_load_from_data(p, json, -1, NULL);
  JsonNode *root_node = ok ? json_parser_get_root(p) : NULL;
  if (root_node == NULL || !JSON_NODE_HOLDS_OBJECT(root_node)) {
    g_object_unref(p);
    return FALSE;
  }
  JsonObject *root = json_node_get_object(root_node);

  PtUsage u;
  pt_usage_clear(&u);
  u.kind = PT_AGENT_CLAUDE;
  g_strlcpy(u.source, "anthropic usage api", sizeof(u.source));

  const char *plan = pt_json_string(root, "subscription_type");
  if (plan == NULL) plan = pt_json_string(root, "plan");
  if (plan == NULL) plan = pt_json_string(root, "plan_type");
  if (plan != NULL) g_strlcpy(u.plan, plan, sizeof(u.plan));

  /* The new shape first: an account that has both would be mid-migration, and
   * the array is the one that survives. */
  JsonArray *arr = pt_json_array(root, "limits");
  gboolean found = arr != NULL ? parse_limits_array(arr, &u)
                               : parse_keyed_windows(root, &u);
  g_object_unref(p);
  /* No recognisable window is a failed lookup, not an unlimited account.
   * Saying "0% used" because the format moved again would be the one failure
   * mode this panel must not have. */
  if (!found) return FALSE;
  *out = u;
  return TRUE;
}
