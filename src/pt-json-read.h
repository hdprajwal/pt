/* pt-json-read.h — forgiving readers for JSON pt did not write.
 *
 * Both usage readers parse documents that belong to somebody else: a session
 * log whose format is an implementation detail, and an endpoint that is not a
 * public API. Neither can be trusted to have the member, the type or even the
 * spelling this build expects, and a missing member has to read as "not there"
 * rather than crash. These wrappers are that: every one takes a fallback and
 * treats a JSON null, a wrong type and an absent member identically. */
#pragma once
#include <json-glib/json-glib.h>

/* TRUE only for a member that exists and is not null. */
gboolean pt_json_is_set(JsonObject *o, const char *name);
/* NULL unless the member is an object / an array. */
JsonObject *pt_json_obj(JsonObject *o, const char *name);
JsonArray  *pt_json_array(JsonObject *o, const char *name);
const char *pt_json_string(JsonObject *o, const char *name);
gint64      pt_json_int(JsonObject *o, const char *name, gint64 fallback);
double      pt_json_double(JsonObject *o, const char *name, double fallback);

/* Like pt_json_double, but says whether it read a number rather than handing
 * back a fallback that cannot be told apart from one.
 *
 * That distinction matters wherever the value is a usage percentage: a member
 * that turns into a string in some later version would otherwise read as 0.0,
 * and a bar drawn at 0% tells the user they have their whole plan left. FALSE
 * has to reach the caller so it can report a failed reading instead. */
gboolean pt_json_number(JsonObject *o, const char *name, double *out);

/* A moment, however it was spelled: an epoch in seconds, an epoch in
 * milliseconds, or an ISO 8601 string. 0 when the node holds none of those.
 *
 * Guessing between the two epoch scales rather than being told is deliberate.
 * Both of these sources have shipped both, and the two are three orders of
 * magnitude apart, so the guess is never close. */
gint64 pt_json_node_epoch(JsonNode *n);
/* The named member, read as above. */
gint64 pt_json_epoch(JsonObject *o, const char *name);
