#include "pt-session.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>

static void tab_state_free(gpointer p) {
  PtTabState *t = p;
  g_free(t->title);
  pt_split_free(t->tree);
  g_free(t);
}

static void project_state_free(gpointer p) {
  PtProjectState *ps = p;
  g_free(ps->name);
  g_free(ps->path);
  g_ptr_array_free(ps->tabs, TRUE);
  g_free(ps);
}

PtSessionState *pt_session_state_new(void) {
  PtSessionState *s = g_new0(PtSessionState, 1);
  s->projects = g_ptr_array_new_with_free_func(project_state_free);
  s->active_project = 0;
  s->font_size = PT_FONT_SIZE_DEFAULT;
  return s;
}

void pt_session_state_free(PtSessionState *s) {
  if (s == NULL) return;
  g_ptr_array_free(s->projects, TRUE);
  g_free(s);
}

PtTabState *pt_tab_state_new(const char *title, PtSplitNode *tree) {
  PtTabState *t = g_new0(PtTabState, 1);
  t->title = g_strdup(title);
  t->tree = tree;
  return t;
}

PtProjectState *pt_project_state_new(const char *name, const char *path) {
  PtProjectState *p = g_new0(PtProjectState, 1);
  p->name = g_strdup(name);
  p->path = g_strdup(path);
  p->tabs = g_ptr_array_new_with_free_func(tab_state_free);
  return p;
}

char *pt_session_to_json_text(const PtSessionState *s) {
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "version");
  json_builder_add_int_value(b, 1);
  json_builder_set_member_name(b, "active_project");
  json_builder_add_int_value(b, s->active_project);
  json_builder_set_member_name(b, "font_size");
  json_builder_add_int_value(b, s->font_size);
  json_builder_set_member_name(b, "projects");
  json_builder_begin_array(b);
  for (guint i = 0; i < s->projects->len; i++) {
    PtProjectState *p = g_ptr_array_index(s->projects, i);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, p->name);
    json_builder_set_member_name(b, "path");
    json_builder_add_string_value(b, p->path);
    json_builder_set_member_name(b, "accent");
    json_builder_add_int_value(b, p->accent);
    json_builder_set_member_name(b, "active_tab");
    json_builder_add_int_value(b, p->active_tab);
    json_builder_set_member_name(b, "tabs");
    json_builder_begin_array(b);
    for (guint j = 0; j < p->tabs->len; j++) {
      PtTabState *t = g_ptr_array_index(p->tabs, j);
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "title");
      json_builder_add_string_value(b, t->title);
      json_builder_set_member_name(b, "splits");
      json_builder_add_value(b, pt_split_to_json(t->tree));
      json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  json_builder_end_object(b);

  JsonGenerator *gen = json_generator_new();
  json_generator_set_pretty(gen, TRUE);
  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(gen, root);
  char *text = json_generator_to_data(gen, NULL);
  json_node_unref(root);
  g_object_unref(gen);
  g_object_unref(b);
  return text;
}

PtSessionState *pt_session_from_json_text(const char *text) {
  if (text == NULL) return NULL;
  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, text, -1, NULL)) {
    g_object_unref(parser);
    return NULL;
  }
  JsonNode *root = json_parser_get_root(parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(parser);
    return NULL;
  }
  JsonObject *obj = json_node_get_object(root);
  PtSessionState *s = pt_session_state_new();
  s->active_project =
      (int)json_object_get_int_member_with_default(obj, "active_project", 0);
  s->font_size =
      (int)json_object_get_int_member_with_default(obj, "font_size",
                                                  PT_FONT_SIZE_DEFAULT);
  JsonArray *projects =
      json_object_has_member(obj, "projects")
          ? json_object_get_array_member(obj, "projects") : NULL;
  for (guint i = 0; projects != NULL && i < json_array_get_length(projects); i++) {
    JsonObject *po = json_array_get_object_element(projects, i);
    if (po == NULL) continue;
    PtProjectState *p = pt_project_state_new(
        json_object_get_string_member_with_default(po, "name", "?"),
        json_object_get_string_member_with_default(po, "path", "/"));
    p->active_tab =
        (int)json_object_get_int_member_with_default(po, "active_tab", 0);
    /* Older state files predate accents: fall back to cycling by position.
       s->projects->len is the index this project is about to occupy, which
       stays correct even if a malformed element above was skipped. */
    gint64 accent = json_object_get_int_member_with_default(
        po, "accent", (gint64)(s->projects->len % PT_ACCENT_COUNT));
    p->accent = (int)CLAMP(accent, 0, PT_ACCENT_COUNT - 1);
    JsonArray *tabs = json_object_has_member(po, "tabs")
                          ? json_object_get_array_member(po, "tabs") : NULL;
    for (guint j = 0; tabs != NULL && j < json_array_get_length(tabs); j++) {
      JsonObject *to = json_array_get_object_element(tabs, j);
      if (to == NULL) continue;
      PtSplitNode *tree =
          pt_split_from_json(json_object_get_member(to, "splits"));
      if (tree == NULL) tree = pt_split_leaf_new(p->path);
      g_ptr_array_add(p->tabs, pt_tab_state_new(
          json_object_get_string_member_with_default(to, "title", "shell"),
          tree));
    }
    g_ptr_array_add(s->projects, p);
  }
  g_object_unref(parser);
  return s;
}

gboolean pt_session_save(const PtSessionState *s, const char *path,
                         GError **err) {
  char *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0700);
  g_free(dir);
  char *text = pt_session_to_json_text(s);
  /* g_file_set_contents writes to a temp file and renames — atomic enough. */
  gboolean ok = g_file_set_contents(path, text, -1, err);
  g_free(text);
  return ok;
}

PtSessionState *pt_session_load(const char *path) {
  char *text = NULL;
  if (!g_file_get_contents(path, &text, NULL, NULL)) return NULL;
  PtSessionState *s = pt_session_from_json_text(text);
  g_free(text);
  if (s == NULL) {
    char *bak = g_strconcat(path, ".bak", NULL);
    g_rename(path, bak);
    /* g_message (not g_warning): GLib's test framework treats warnings as
       fatal, and a corrupt-file recovery is an expected, handled condition. */
    g_message("pt: corrupt state file moved to %s", bak);
    g_free(bak);
  }
  return s;
}

char *pt_session_default_path(void) {
  return g_build_filename(g_get_user_config_dir(), "pt", "state.json", NULL);
}

int pt_session_index_after_move(int index, int from, int to) {
  if (from == to || index < 0) return index;
  if (index == from) return to;
  /* Everything the moved project passes over shifts one slot the other way;
   * everything outside [from, to] keeps its position. */
  if (from < to) return (index > from && index <= to) ? index - 1 : index;
  return (index >= to && index < from) ? index + 1 : index;
}
