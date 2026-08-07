#include "pt-json-read.h"

gboolean pt_json_is_set(JsonObject *o, const char *name) {
  if (o == NULL || name == NULL || !json_object_has_member(o, name))
    return FALSE;
  JsonNode *n = json_object_get_member(o, name);
  return n != NULL && !JSON_NODE_HOLDS_NULL(n);
}

/* Every reader below funnels through this: "set, and of the type I am about
 * to read it as". Without the type check json-glib's getters abort on a
 * mismatch, which would turn a renamed field in someone else's format into a
 * crash in pt. */
static JsonNode *node_of(JsonObject *o, const char *name, JsonNodeType type) {
  if (!pt_json_is_set(o, name)) return NULL;
  JsonNode *n = json_object_get_member(o, name);
  return json_node_get_node_type(n) == type ? n : NULL;
}

JsonObject *pt_json_obj(JsonObject *o, const char *name) {
  JsonNode *n = node_of(o, name, JSON_NODE_OBJECT);
  return n != NULL ? json_node_get_object(n) : NULL;
}

JsonArray *pt_json_array(JsonObject *o, const char *name) {
  JsonNode *n = node_of(o, name, JSON_NODE_ARRAY);
  return n != NULL ? json_node_get_array(n) : NULL;
}

const char *pt_json_string(JsonObject *o, const char *name) {
  JsonNode *n = node_of(o, name, JSON_NODE_VALUE);
  if (n == NULL) return NULL;
  /* A value node that is a number answers NULL here rather than asserting. */
  return json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n)
                                                      : NULL;
}

gint64 pt_json_int(JsonObject *o, const char *name, gint64 fallback) {
  JsonNode *n = node_of(o, name, JSON_NODE_VALUE);
  if (n == NULL) return fallback;
  GType t = json_node_get_value_type(n);
  if (t == G_TYPE_INT64) return json_node_get_int(n);
  if (t == G_TYPE_DOUBLE) return (gint64)json_node_get_double(n);
  return fallback;
}

gboolean pt_json_number(JsonObject *o, const char *name, double *out) {
  JsonNode *n = node_of(o, name, JSON_NODE_VALUE);
  if (n == NULL) return FALSE;
  GType t = json_node_get_value_type(n);
  if (t == G_TYPE_DOUBLE)     *out = json_node_get_double(n);
  else if (t == G_TYPE_INT64) *out = (double)json_node_get_int(n);
  else return FALSE;
  return TRUE;
}

double pt_json_double(JsonObject *o, const char *name, double fallback) {
  double v;
  return pt_json_number(o, name, &v) ? v : fallback;
}

gint64 pt_json_node_epoch(JsonNode *n) {
  if (n == NULL || !JSON_NODE_HOLDS_VALUE(n)) return 0;
  GType t = json_node_get_value_type(n);
  if (t == G_TYPE_STRING) {
    const char *s = json_node_get_string(n);
    if (s == NULL || s[0] == '\0') return 0;
    GDateTime *dt = g_date_time_new_from_iso8601(s, NULL);
    if (dt == NULL) return 0;
    gint64 unix_s = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    return unix_s;
  }
  gint64 v = 0;
  if (t == G_TYPE_INT64)       v = json_node_get_int(n);
  else if (t == G_TYPE_DOUBLE) v = (gint64)json_node_get_double(n);
  else return 0;
  /* Seconds this large land past the year 5138, milliseconds this large land
   * in 1973 — so the two ranges do not overlap anywhere near now. */
  return v > 100000000000LL ? v / 1000 : v;
}

gint64 pt_json_epoch(JsonObject *o, const char *name) {
  if (!pt_json_is_set(o, name)) return 0;
  return pt_json_node_epoch(json_object_get_member(o, name));
}
