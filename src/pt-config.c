#include "pt-config.h"
#include <glib/gstdio.h>   /* g_mkdir_with_parents */
#include <stdlib.h>
#include <string.h>

void pt_kv_parse(const char *text, PtKvFn fn, gpointer user) {
  if (text == NULL) return;
  char **lines = g_strsplit(text, "\n", -1);
  for (int i = 0; lines[i] != NULL; i++) {
    char *line = g_strstrip(g_strdup(lines[i]));
    if (line[0] == '\0' || line[0] == '#') { g_free(line); continue; }
    char *eq = strchr(line, '=');
    if (eq == NULL) {
      fn(line, NULL, i + 1, user);   /* malformed: whole line as key */
      g_free(line);
      continue;
    }
    *eq = '\0';
    char *key = g_strstrip(line);
    char *value = g_strstrip(eq + 1);
    fn(key, value, i + 1, user);
    g_free(line);
  }
  g_strfreev(lines);
}

PtConfig *pt_config_new(void) {
  PtConfig *c = g_new0(PtConfig, 1);
  c->theme = g_strdup(PT_CONFIG_THEME_DEFAULT);
  c->font_size = PT_CONFIG_FONT_SIZE_DEFAULT;
  c->font_family = g_strdup(PT_CONFIG_FONT_FAMILY_DEFAULT);
  c->ui_font_size = PT_CONFIG_UI_FONT_SIZE_DEFAULT;
  c->ui_font_family = g_strdup(PT_CONFIG_UI_FONT_FAMILY_DEFAULT);
  c->mouse_reporting = PT_CONFIG_MOUSE_REPORTING_DEFAULT;
  c->osc52 = PT_CONFIG_OSC52_DEFAULT;
  c->app_overrides = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, g_free);
  return c;
}

void pt_config_free(PtConfig *c) {
  if (c == NULL) return;
  g_free(c->theme);
  g_free(c->font_family);
  g_free(c->ui_font_family);
  g_hash_table_unref(c->app_overrides);
  g_free(c);
}

PtConfig *pt_config_copy(const PtConfig *c) {
  PtConfig *n = pt_config_new();
  g_free(n->theme);          n->theme = g_strdup(c->theme);
  g_free(n->font_family);    n->font_family = g_strdup(c->font_family);
  g_free(n->ui_font_family); n->ui_font_family = g_strdup(c->ui_font_family);
  n->font_size = c->font_size;
  n->ui_font_size = c->ui_font_size;
  n->mouse_reporting = c->mouse_reporting;
  n->osc52 = c->osc52;
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, c->app_overrides);
  while (g_hash_table_iter_next(&it, &k, &v))
    g_hash_table_insert(n->app_overrides, g_strdup(k), g_strdup(v));
  return n;
}

static gboolean tables_equal(GHashTable *a, GHashTable *b) {
  if (g_hash_table_size(a) != g_hash_table_size(b)) return FALSE;
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, a);
  while (g_hash_table_iter_next(&it, &k, &v))
    if (g_strcmp0(g_hash_table_lookup(b, k), v) != 0) return FALSE;
  return TRUE;
}

gboolean pt_config_equal(const PtConfig *a, const PtConfig *b) {
  return g_strcmp0(a->theme, b->theme) == 0 &&
         a->font_size == b->font_size &&
         g_strcmp0(a->font_family, b->font_family) == 0 &&
         a->ui_font_size == b->ui_font_size &&
         g_strcmp0(a->ui_font_family, b->ui_font_family) == 0 &&
         a->mouse_reporting == b->mouse_reporting &&
         a->osc52 == b->osc52 &&
         tables_equal(a->app_overrides, b->app_overrides);
}

/* Booleans are spelled the way people already spell them in dotfiles; anything
 * else warns and keeps the default, like every other value here. */
static gboolean parse_bool(const char *value, gboolean *out) {
  static const char *yes[] = { "true", "yes", "on", "1" };
  static const char *no[]  = { "false", "no", "off", "0" };
  for (gsize i = 0; i < G_N_ELEMENTS(yes); i++)
    if (g_ascii_strcasecmp(value, yes[i]) == 0) { *out = TRUE; return TRUE; }
  for (gsize i = 0; i < G_N_ELEMENTS(no); i++)
    if (g_ascii_strcasecmp(value, no[i]) == 0) { *out = FALSE; return TRUE; }
  return FALSE;
}

static const char *const osc52_names[] = { "off", "write", "ask" };

static gboolean parse_osc52(const char *value, PtOsc52Mode *out) {
  for (gsize i = 0; i < G_N_ELEMENTS(osc52_names); i++)
    if (g_ascii_strcasecmp(value, osc52_names[i]) == 0) {
      *out = (PtOsc52Mode)i;
      return TRUE;
    }
  return FALSE;
}

static void on_kv(const char *key, const char *value, int lineno,
                  gpointer user) {
  PtConfig *c = user;
  if (value == NULL) {
    g_warning("pt: config line %d: no '=' — skipped", lineno);
    return;
  }
  if (g_strcmp0(key, "theme") == 0 && value[0] != '\0') {
    g_free(c->theme);
    c->theme = g_strdup(value);
  } else if (g_strcmp0(key, "font-size") == 0) {
    char *end = NULL;
    long v = strtol(value, &end, 10);
    /* Stay permissive but sane: reject <= 0 and absurd values so the int
     * narrowing below can never wrap. The UI clamps to its own tighter
     * range later. */
    if (end != value && *end == '\0' && v >= PT_CONFIG_FONT_SIZE_MIN &&
        v <= PT_CONFIG_FONT_SIZE_MAX)
      c->font_size = (int)v;
    else g_warning("pt: config line %d: bad font-size '%s'", lineno, value);
  } else if (g_strcmp0(key, "font-family") == 0 && value[0] != '\0') {
    g_free(c->font_family);
    c->font_family = g_strdup(value);
  } else if (g_strcmp0(key, "ui-font-size") == 0) {
    char *end = NULL;
    double v = g_ascii_strtod(value, &end);
    if (end != value && *end == '\0' && v > 0) c->ui_font_size = v;
    else g_warning("pt: config line %d: bad ui-font-size '%s'", lineno, value);
  } else if (g_strcmp0(key, "ui-font-family") == 0 && value[0] != '\0') {
    g_free(c->ui_font_family);
    c->ui_font_family = g_strdup(value);
  } else if (g_strcmp0(key, "mouse-reporting") == 0) {
    gboolean v = FALSE;
    if (parse_bool(value, &v)) c->mouse_reporting = v;
    else g_warning("pt: config line %d: bad mouse-reporting '%s'", lineno,
                   value);
  } else if (g_strcmp0(key, "osc52") == 0) {
    PtOsc52Mode m = PT_CONFIG_OSC52_DEFAULT;
    if (parse_osc52(value, &m)) c->osc52 = m;
    else g_warning("pt: config line %d: bad osc52 '%s' (off, write or ask)",
                   lineno, value);
  } else if (g_str_has_prefix(key, "app-") && key[4] != '\0') {
    g_hash_table_insert(c->app_overrides, g_strdup(key + 4), g_strdup(value));
  }
  /* Unknown keys: ignored on read, preserved by rewrite. */
}

PtConfig *pt_config_parse(const char *text) {
  PtConfig *c = pt_config_new();
  pt_kv_parse(text, on_kv, c);
  return c;
}

char *pt_config_default_path(void) {
  return g_build_filename(g_get_user_config_dir(), "pt", "config", NULL);
}

/* ---- in-place rewrite ---- */
typedef struct { const char *key; char *value; gboolean written; } Managed;

/* Keys the app writes back. managed_value() must cover every index. */
#define PT_MANAGED_KEYS 7

static char *managed_value(const PtConfig *c, int i) {
  switch (i) {
    case 0: return g_strdup(c->theme);
    case 1: return g_strdup_printf("%d", c->font_size);
    case 2: return g_strdup(c->font_family);
    case 3: {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];
      g_ascii_formatd(buf, sizeof buf, "%g", c->ui_font_size);
      return g_strdup(buf);
    }
    case 4: return g_strdup(c->ui_font_family);
    case 5: return g_strdup(c->mouse_reporting ? "true" : "false");
    case 6: return g_strdup(osc52_names[c->osc52]);
  }
  return NULL;
}

char *pt_config_rewrite(const char *old_text, const PtConfig *c) {
  Managed keys[PT_MANAGED_KEYS] = {
    { "theme", NULL, FALSE },      { "font-size", NULL, FALSE },
    { "font-family", NULL, FALSE },{ "ui-font-size", NULL, FALSE },
    { "ui-font-family", NULL, FALSE }, { "mouse-reporting", NULL, FALSE },
    { "osc52", NULL, FALSE },
  };
  for (int i = 0; i < PT_MANAGED_KEYS; i++) keys[i].value = managed_value(c, i);

  GString *out = g_string_new(NULL);
  char **lines = g_strsplit(old_text != NULL ? old_text : "", "\n", -1);
  int n_lines = 0;
  while (lines[n_lines] != NULL) n_lines++;
  /* g_strsplit gives a trailing "" when the text ends in \n; drop it so we
   * control the final newline ourselves. */
  int last = (n_lines > 0 && lines[n_lines - 1][0] == '\0') ? n_lines - 1
                                                            : n_lines;
  for (int i = 0; i < last; i++) {
    char *probe = g_strstrip(g_strdup(lines[i]));
    gboolean replaced = FALSE;
    if (probe[0] != '\0' && probe[0] != '#') {
      char *eq = strchr(probe, '=');
      if (eq != NULL) {
        *eq = '\0';
        char *key = g_strstrip(probe);
        for (int k = 0; k < PT_MANAGED_KEYS; k++) {
          if (g_strcmp0(key, keys[k].key) == 0) {
            g_string_append_printf(out, "%s = %s\n",
                                   keys[k].key, keys[k].value);
            keys[k].written = TRUE;
            replaced = TRUE;
            break;
          }
        }
      }
    }
    if (!replaced) g_string_append_printf(out, "%s\n", lines[i]);
    g_free(probe);
  }
  g_strfreev(lines);
  for (int k = 0; k < PT_MANAGED_KEYS; k++) {
    if (!keys[k].written)
      g_string_append_printf(out, "%s = %s\n", keys[k].key, keys[k].value);
    g_free(keys[k].value);
  }
  return g_string_free(out, FALSE);
}

/* ---- disk ---- */
PtConfig *pt_config_load(const char *path) {
  char *text = NULL;
  if (!g_file_get_contents(path, &text, NULL, NULL))
    return pt_config_new();
  PtConfig *c = pt_config_parse(text);
  g_free(text);
  return c;
}

gboolean pt_config_save(const PtConfig *c, const char *path, GError **err) {
  char *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0700);
  g_free(dir);
  char *old = NULL;
  g_file_get_contents(path, &old, NULL, NULL);  /* absent is fine */
  char *text = pt_config_rewrite(old != NULL ? old : "", c);
  gboolean ok = g_file_set_contents(path, text, -1, err);
  g_free(old);
  g_free(text);
  return ok;
}
