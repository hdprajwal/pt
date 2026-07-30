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

/* ---- per-project field table ----
 * The scalar members of a project are the same list going out and coming back,
 * so both directions walk one table: a new field is one row, and no field can
 * be saved without also being restored. Two things stay by hand below: `accent`,
 * whose default depends on the project's own position and which is clamped to
 * the accent cycle, and `tabs`, which is an array of objects rather than a
 * scalar. */
typedef enum { PFLD_STR, PFLD_INT } PtProjectFieldType;

typedef struct {
  const char *member;
  size_t offset;            /* into PtProjectState */
  PtProjectFieldType type;
  const char *fallback_str; /* PFLD_STR: used when the member is missing */
  int fallback_int;         /* PFLD_INT: ditto */
} PtProjectField;

static const PtProjectField project_fields[] = {
  { "name",       G_STRUCT_OFFSET(PtProjectState, name),       PFLD_STR,
    "?", 0 },
  { "path",       G_STRUCT_OFFSET(PtProjectState, path),       PFLD_STR,
    "/", 0 },
  { "active_tab", G_STRUCT_OFFSET(PtProjectState, active_tab), PFLD_INT,
    NULL, 0 },
};

static void project_field_write(const PtProjectField *f,
                                const PtProjectState *p, JsonBuilder *b) {
  gconstpointer slot = (const guint8 *)p + f->offset;
  json_builder_set_member_name(b, f->member);
  switch (f->type) {
    case PFLD_STR:
      json_builder_add_string_value(b, *(const char *const *)slot);
      break;
    case PFLD_INT:
      json_builder_add_int_value(b, *(const int *)slot);
      break;
  }
}

static void project_field_read(const PtProjectField *f, PtProjectState *p,
                               JsonObject *o) {
  gpointer slot = (guint8 *)p + f->offset;
  switch (f->type) {
    case PFLD_STR: {
      char **str = slot;
      g_free(*str);
      *str = g_strdup(json_object_get_string_member_with_default(
          o, f->member, f->fallback_str));
      break;
    }
    case PFLD_INT:
      *(int *)slot = (int)json_object_get_int_member_with_default(
          o, f->member, f->fallback_int);
      break;
  }
}

char *pt_session_to_json_text(const PtSessionState *s) {
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "version");
  json_builder_add_int_value(b, PT_SESSION_VERSION);
  json_builder_set_member_name(b, "active_project");
  json_builder_add_int_value(b, s->active_project);
  json_builder_set_member_name(b, "font_size");
  json_builder_add_int_value(b, s->font_size);
  json_builder_set_member_name(b, "projects");
  json_builder_begin_array(b);
  for (guint i = 0; i < s->projects->len; i++) {
    PtProjectState *p = g_ptr_array_index(s->projects, i);
    json_builder_begin_object(b);
    for (gsize f = 0; f < G_N_ELEMENTS(project_fields); f++)
      project_field_write(&project_fields[f], p, b);
    json_builder_set_member_name(b, "accent");
    json_builder_add_int_value(b, p->accent);
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
  /* A file from a newer pt may mean anything by these members; refuse it the
     way we refuse malformed JSON instead of guessing. Files written before the
     field existed, and files at our own version, read normally. */
  gint64 version =
      json_object_get_int_member_with_default(obj, "version",
                                              PT_SESSION_VERSION);
  if (version > PT_SESSION_VERSION) {
    g_message("pt: state file is version %" G_GINT64_FORMAT ", this pt reads %d",
              version, PT_SESSION_VERSION);
    g_object_unref(parser);
    return NULL;
  }
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
    /* The table writes name and path, falling back to "?" and "/", so both are
       non-NULL once the field loop below has run. */
    PtProjectState *p = pt_project_state_new(NULL, NULL);
    for (gsize f = 0; f < G_N_ELEMENTS(project_fields); f++)
      project_field_read(&project_fields[f], p, po);
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

/* Move an unreadable state file out of the way so the next launch starts clean,
   without ever destroying an earlier rescue: the first file we saved aside is
   the one most likely to hold the session someone wants back, so `.bak` is
   written once and then kept. A later casualty goes to `.bak.1`, which may be
   overwritten — one slot of history is enough to look at, and an unbounded
   chain of backups is its own mess. */
static void move_aside(const char *path) {
  char *bak = g_strconcat(path, ".bak", NULL);
  if (g_file_test(bak, G_FILE_TEST_EXISTS)) {
    g_free(bak);
    bak = g_strconcat(path, ".bak.1", NULL);
  }
  /* g_message (not g_warning): GLib's test framework treats warnings as
     fatal, and a corrupt-file recovery is an expected, handled condition. */
  if (g_rename(path, bak) == 0)
    g_message("pt: unreadable state file moved to %s", bak);
  else
    g_message("pt: could not move unreadable state file %s aside", path);
  g_free(bak);
}

PtSessionState *pt_session_load(const char *path) {
  char *text = NULL;
  if (!g_file_get_contents(path, &text, NULL, NULL)) return NULL;
  PtSessionState *s = pt_session_from_json_text(text);
  g_free(text);
  if (s == NULL) move_aside(path);
  return s;
}

char *pt_session_default_path(void) {
  return g_build_filename(g_get_user_config_dir(), "pt", "state.json", NULL);
}
