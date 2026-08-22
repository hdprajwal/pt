#include "pt-config.h"
#include <glib/gstdio.h>   /* g_mkdir_with_parents */
#include <math.h>          /* isfinite */
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

/* One raw `bind`/`unbind` line, kept verbatim (verb included) with where it
 * sat in the file. Parsing into accelerators and actions is pt-bindings' job;
 * this is only collection. */
typedef struct {
  char *text;
  int line_no;
} PtBindingSpec;

static void binding_spec_free(gpointer p) {
  PtBindingSpec *s = p;
  g_free(s->text);
  g_free(s);
}

PtConfig *pt_config_new(void) {
  PtConfig *c = g_new0(PtConfig, 1);
  c->theme = g_strdup(PT_CONFIG_THEME_DEFAULT);
  c->font_size = PT_CONFIG_FONT_SIZE_DEFAULT;
  c->font_family = g_strdup(PT_CONFIG_FONT_FAMILY_DEFAULT);
  c->ui_font_size = PT_CONFIG_UI_FONT_SIZE_DEFAULT;
  c->ui_font_family = g_strdup(PT_CONFIG_UI_FONT_FAMILY_DEFAULT);
  c->term = g_strdup(PT_CONFIG_TERM_DEFAULT);
  c->scrollback_limit = PT_CONFIG_SCROLLBACK_LIMIT_DEFAULT;
  c->window_padding_x = PT_CONFIG_WINDOW_PADDING_X_DEFAULT;
  c->window_padding_y = PT_CONFIG_WINDOW_PADDING_Y_DEFAULT;
  c->mouse_reporting = PT_CONFIG_MOUSE_REPORTING_DEFAULT;
  c->claude_usage = PT_CONFIG_CLAUDE_USAGE_DEFAULT;
  c->resume_agents = PT_CONFIG_RESUME_AGENTS_DEFAULT;
  c->osc52 = PT_CONFIG_OSC52_DEFAULT;
  c->bell = PT_CONFIG_BELL_DEFAULT;
  c->app_overrides = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, g_free);
  c->binding_lines =
      g_ptr_array_new_with_free_func((GDestroyNotify)binding_spec_free);
  return c;
}

void pt_config_free(PtConfig *c) {
  if (c == NULL) return;
  g_free(c->theme);
  g_free(c->font_family);
  g_free(c->ui_font_family);
  g_free(c->term);
  g_hash_table_unref(c->app_overrides);
  g_ptr_array_unref(c->binding_lines);
  g_free(c);
}

gboolean pt_bell_visual(PtBellMode mode) {
  return mode == PT_BELL_VISUAL || mode == PT_BELL_BOTH;
}

gboolean pt_bell_audio(PtBellMode mode) {
  return mode == PT_BELL_AUDIBLE || mode == PT_BELL_BOTH;
}

gboolean pt_bell_attention(gboolean pane_focused, PtBellMode mode) {
  return !pane_focused && pt_bell_visual(mode);
}

gboolean pt_bell_audio_take(gint64 *last, gint64 now) {
  if (now - *last < PT_BELL_AUDIO_INTERVAL_USEC) return FALSE;
  *last = now;
  return TRUE;
}

/* ---- field table ----
 * One row per key the app reads and writes back. Parsing, copy, equality and
 * the in-place rewrite all walk this table, so a new setting is one row plus
 * its struct member and default — no three lists to keep in step. app_overrides
 * is the one exception: it is a key *prefix*, not a key, so it stays by hand in
 * on_kv and pt_config_copy. */
typedef enum { FLD_STR, FLD_INT, FLD_DOUBLE, FLD_BOOL, FLD_ENUM } PtFieldType;

/* Which typed slot an FLD_ENUM row fronts. The two enum settings have
 * distinct types (PtOsc52Mode, PtBellMode), so the generic table cannot
 * hold one pointer type for both; every store and read goes through the
 * field's own type via enum_store / enum_load rather than punning int. */
typedef enum { PT_ENUM_OSC52, PT_ENUM_BELL } PtEnumId;

typedef struct {
  const char *key;
  size_t offset;             /* into PtConfig */
  PtFieldType type;
  /* The range the parser accepts; anything outside it warns and keeps the
   * default. FLD_INT is inclusive at both ends. FLD_DOUBLE's low end is
   * exclusive and its values must be finite, so `min = 0` reads as "any
   * positive real" — see field_parse. */
  double min, max;
  const char *const *names;  /* FLD_ENUM: NULL-terminated spellings, in order */
  const char *hint;          /* spelled out in the warning when non-NULL */
  PtEnumId enum_id;          /* FLD_ENUM only */
} PtConfigField;

static const char *const osc52_names[] = { "off", "write", "ask", NULL };
static const char *const bell_names[] = { "visual", "audible", "both", "off",
                                          NULL };

static const PtConfigField config_fields[] = {
  { "theme",           G_STRUCT_OFFSET(PtConfig, theme),
    FLD_STR,    0, 0, NULL, NULL },
  /* Stay permissive but sane: the range keeps the narrowing to int from ever
   * wrapping. The UI clamps to its own tighter range later. */
  { "font-size",       G_STRUCT_OFFSET(PtConfig, font_size),
    FLD_INT,    PT_CONFIG_FONT_SIZE_MIN, PT_CONFIG_FONT_SIZE_MAX, NULL, NULL },
  { "font-family",     G_STRUCT_OFFSET(PtConfig, font_family),
    FLD_STR,    0, 0, NULL, NULL },
  /* Any positive, finite size: this one ends up in a CSS length. */
  { "ui-font-size",    G_STRUCT_OFFSET(PtConfig, ui_font_size),
    FLD_DOUBLE, 0, G_MAXDOUBLE, NULL, NULL },
  { "ui-font-family",  G_STRUCT_OFFSET(PtConfig, ui_font_family),
    FLD_STR,    0, 0, NULL, NULL },
  /* Any string: what a terminfo entry may be called is not this parser's
   * business, and a name with no entry behind it falls back at spawn time
   * rather than at parse time — see term_name() in pt-term-core.c. */
  { "term",            G_STRUCT_OFFSET(PtConfig, term),
    FLD_STR,    0, 0, NULL, NULL },
  { "mouse-reporting", G_STRUCT_OFFSET(PtConfig, mouse_reporting),
    FLD_BOOL,   0, 0, NULL, NULL },
  { "claude-usage",    G_STRUCT_OFFSET(PtConfig, claude_usage),
    FLD_BOOL,   0, 0, NULL, NULL },
  { "resume-agents",   G_STRUCT_OFFSET(PtConfig, resume_agents),
    FLD_BOOL,   0, 0, NULL, NULL },
  { "osc52",           G_STRUCT_OFFSET(PtConfig, osc52),
    FLD_ENUM,   0, 0, osc52_names, "off, write or ask", PT_ENUM_OSC52 },
  { "bell",            G_STRUCT_OFFSET(PtConfig, bell),
    FLD_ENUM,   0, 0, bell_names, "visual, audible, both or off",
    PT_ENUM_BELL },
  /* Bytes of history, and the whole int range of them: 0 is a pane that keeps
   * none, which libghostty accepts, and the top end is far past what any pane
   * can fill. */
  { "scrollback-limit", G_STRUCT_OFFSET(PtConfig, scrollback_limit),
    FLD_INT,    0, G_MAXINT, NULL, "bytes, zero or more" },
  /* Pixels of inset around the grid. Zero is a pane whose text starts at its
   * own edge, which is a real taste; the ceiling stops a value that would
   * leave nothing to put a grid in. */
  { "window-padding-x", G_STRUCT_OFFSET(PtConfig, window_padding_x),
    FLD_INT,    PT_CONFIG_WINDOW_PADDING_MIN, PT_CONFIG_WINDOW_PADDING_MAX,
    NULL, "pixels, 0 to 200" },
  { "window-padding-y", G_STRUCT_OFFSET(PtConfig, window_padding_y),
    FLD_INT,    PT_CONFIG_WINDOW_PADDING_MIN, PT_CONFIG_WINDOW_PADDING_MAX,
    NULL, "pixels, 0 to 200" },
};

static gpointer field_slot(PtConfig *c, const PtConfigField *f) {
  return (guint8 *)c + f->offset;
}

static gconstpointer field_slot_const(const PtConfig *c,
                                      const PtConfigField *f) {
  return (const guint8 *)c + f->offset;
}

/* The enum slots are stored as their real types, so every access goes
 * through a pointer of that type. `i` is an index into the field's names,
 * which the enum values mirror in order. */
static void enum_store(const PtConfigField *f, gpointer p, gsize i) {
  if (f->enum_id == PT_ENUM_OSC52)
    *(PtOsc52Mode *)p = (PtOsc52Mode)i;
  else
    *(PtBellMode *)p = (PtBellMode)i;
}

static gsize enum_load(const PtConfigField *f, gconstpointer p) {
  return f->enum_id == PT_ENUM_OSC52 ? (gsize)*(const PtOsc52Mode *)p
                                     : (gsize)*(const PtBellMode *)p;
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

/* FALSE means "unusable value": the caller warns and the default stands. */
static gboolean field_parse(const PtConfigField *f, PtConfig *c,
                            const char *value) {
  gpointer p = field_slot(c, f);
  switch (f->type) {
    case FLD_STR: {
      char **slot = p;
      g_free(*slot);
      *slot = g_strdup(value);
      return TRUE;
    }
    case FLD_INT: {
      char *end = NULL;
      long v = strtol(value, &end, 10);
      if (end == value || *end != '\0' || (double)v < f->min ||
          (double)v > f->max)
        return FALSE;
      *(int *)p = (int)v;
      return TRUE;
    }
    case FLD_DOUBLE: {
      char *end = NULL;
      double v = g_ascii_strtod(value, &end);
      /* isfinite first, and on its own: strtod spells `nan` and `inf` as
       * numbers, and NaN compares false against every bound, so a range test
       * alone lets `ui-font-size = nan` through and puts "nanpx" in the CSS.
       * The low end is exclusive rather than clamped to G_MINDOUBLE so the
       * smallest positive value a double can hold still counts — what a size
       * has to be is positive, not normalised. */
      if (end == value || *end != '\0' || !isfinite(v) || v <= f->min ||
          v > f->max)
        return FALSE;
      *(double *)p = v;
      return TRUE;
    }
    case FLD_BOOL:
      return parse_bool(value, p);
    case FLD_ENUM:
      for (gsize i = 0; f->names[i] != NULL; i++)
        if (g_ascii_strcasecmp(value, f->names[i]) == 0) {
          /* Every enum here is small and non-negative, so the index is the
           * value on all the platforms GTK builds for; the enum types are
           * named in enum_store, where they buy a real check. */
          enum_store(f, p, i);
          return TRUE;
        }
      return FALSE;
  }
  return FALSE;
}

/* How the value is spelled back into the config file. Caller frees. */
static char *field_value(const PtConfigField *f, const PtConfig *c) {
  gconstpointer p = field_slot_const(c, f);
  switch (f->type) {
    case FLD_STR:    return g_strdup(*(const char *const *)p);
    case FLD_INT:    return g_strdup_printf("%d", *(const int *)p);
    case FLD_DOUBLE: {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];
      g_ascii_formatd(buf, sizeof buf, "%g", *(const double *)p);
      return g_strdup(buf);
    }
    case FLD_BOOL:   return g_strdup(*(const gboolean *)p ? "true" : "false");
    case FLD_ENUM: {
      gsize n = 0;
      while (f->names[n] != NULL) n++;
      gsize i = enum_load(f, p);
      /* The parser can only produce values the names cover; one set from
       * anywhere else writes the first spelling rather than reading past the
       * list into whatever follows it. */
      return g_strdup(f->names[i < n ? i : 0]);
    }
  }
  return NULL;
}

static void field_copy(const PtConfigField *f, PtConfig *dst,
                       const PtConfig *src) {
  gpointer d = field_slot(dst, f);
  gconstpointer s = field_slot_const(src, f);
  switch (f->type) {
    case FLD_STR:
      g_free(*(char **)d);
      *(char **)d = g_strdup(*(const char *const *)s);
      break;
    case FLD_INT:    *(int *)d = *(const int *)s; break;
    case FLD_DOUBLE: *(double *)d = *(const double *)s; break;
    case FLD_BOOL:   *(gboolean *)d = *(const gboolean *)s; break;
    case FLD_ENUM:   enum_store(f, d, enum_load(f, s)); break;
  }
}

static gboolean field_equal(const PtConfigField *f, const PtConfig *a,
                            const PtConfig *b) {
  gconstpointer x = field_slot_const(a, f);
  gconstpointer y = field_slot_const(b, f);
  switch (f->type) {
    case FLD_STR:
      return g_strcmp0(*(const char *const *)x, *(const char *const *)y) == 0;
    case FLD_INT:    return *(const int *)x == *(const int *)y;
    case FLD_DOUBLE: return *(const double *)x == *(const double *)y;
    case FLD_BOOL:   return *(const gboolean *)x == *(const gboolean *)y;
    case FLD_ENUM:   return enum_load(f, x) == enum_load(f, y);
  }
  return FALSE;
}

PtConfig *pt_config_copy(const PtConfig *c) {
  PtConfig *n = pt_config_new();
  for (gsize i = 0; i < G_N_ELEMENTS(config_fields); i++)
    field_copy(&config_fields[i], n, c);
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, c->app_overrides);
  while (g_hash_table_iter_next(&it, &k, &v))
    g_hash_table_insert(n->app_overrides, g_strdup(k), g_strdup(v));
  for (guint i = 0; i < c->binding_lines->len; i++) {
    const PtBindingSpec *s = g_ptr_array_index(c->binding_lines, i);
    PtBindingSpec *copy = g_new0(PtBindingSpec, 1);
    copy->text = g_strdup(s->text);
    copy->line_no = s->line_no;
    g_ptr_array_add(n->binding_lines, copy);
  }
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
  for (gsize i = 0; i < G_N_ELEMENTS(config_fields); i++)
    if (!field_equal(&config_fields[i], a, b)) return FALSE;
  if (!tables_equal(a->app_overrides, b->app_overrides)) return FALSE;
  if (a->binding_lines->len != b->binding_lines->len) return FALSE;
  /* Text only: where the lines sat in the file is not part of what they say. */
  for (guint i = 0; i < a->binding_lines->len; i++)
    if (g_strcmp0(pt_config_binding_line(a, i),
                  pt_config_binding_line(b, i)) != 0)
      return FALSE;
  return TRUE;
}

/* `key` starts with `verb` followed by whitespace: the rest of it, with that
 * whitespace skipped. NULL when it does not, so `bind` alone and `binder`
 * both decline. Tabs separate as well as spaces — a bind line is words, and
 * a tab between two of them is not worth refusing over.
 *
 * Read-only on purpose. `key` points into pt_kv_parse's own buffer and
 * pt-theme walks that same callback, so this trims by moving a pointer
 * rather than writing through a const one. Only leading whitespace can be
 * left to trim anyway: the line arrived stripped. */
static const char *verb_rest(const char *key, const char *verb) {
  gsize n = strlen(verb);
  if (strncmp(key, verb, n) != 0) return NULL;
  const char *p = key + n;
  if (*p != ' ' && *p != '\t') return NULL;
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

/* Bindings are collected raw: what an accelerator and an action name are is
 * pt-bindings' grammar, not this file's. A bind line carries no '=' on
 * purpose — `=` is itself a bindable key — so it arrives through the
 * malformed-line path below with the whole line as its key; the `bind =`
 * spelling works too, for anyone whose fingers type one anyway. */
static void collect_binding(PtConfig *c, const char *key, const char *value,
                            int lineno) {
  gboolean verb_bind = g_strcmp0(key, "bind") == 0;
  gboolean verb_unbind = g_strcmp0(key, "unbind") == 0;
  if (!verb_bind && !verb_unbind) return;
  /* An empty value has nothing to bind, and unlike every managed string key
   * it cannot read as "leave this alone". */
  if (value == NULL || value[0] == '\0') {
    g_warning("pt: config line %d: %s needs an accelerator — skipped",
              lineno, key);
    return;
  }
  PtBindingSpec *s = g_new0(PtBindingSpec, 1);
  s->text = g_strdup_printf("%s %s", key, value);
  s->line_no = lineno;
  g_ptr_array_add(c->binding_lines, s);
}

static void on_kv(const char *key, const char *value, int lineno,
                  gpointer user) {
  PtConfig *c = user;
  if (value == NULL) {
    /* No '=' anywhere: either a bare `bind <accel> [action]` line, which is
     * spelled without one on purpose, or a genuinely malformed line. The
     * line arrives stripped, so a lone `bind` with nothing after it means
     * exactly what an empty value would have. */
    if (g_strcmp0(key, "bind") == 0 || g_strcmp0(key, "unbind") == 0) {
      collect_binding(c, key, "", lineno);
      return;
    }
    const char *rest = verb_rest(key, "bind");
    if (rest != NULL) {
      collect_binding(c, "bind", rest, lineno);
      return;
    }
    rest = verb_rest(key, "unbind");
    if (rest != NULL) {
      collect_binding(c, "unbind", rest, lineno);
      return;
    }
    g_warning("pt: config line %d: no '=' — skipped", lineno);
    return;
  }
  collect_binding(c, key, value, lineno);
  /* A bind line whose accelerator carries a literal '=' split on it up in
   * pt_kv_parse, so the verb and the front of the accelerator arrived as the
   * key and the rest as a value. Nothing above matched it and nothing below
   * will. Say why rather than letting `bind ctrl+= font-zoom-in` vanish: the
   * grammar spells punctuation by name, and `equal` is the name. */
  if (verb_rest(key, "bind") != NULL || verb_rest(key, "unbind") != NULL) {
    g_warning("pt: config line %d: '=' in a bind line — spell punctuation by "
              "name (equal, plus, minus) — skipped", lineno);
    return;
  }
  for (gsize i = 0; i < G_N_ELEMENTS(config_fields); i++) {
    const PtConfigField *f = &config_fields[i];
    if (g_strcmp0(key, f->key) != 0) continue;
    /* An empty string is not a font name or a theme: keep the default, and
     * quietly, since `key =` reads as "leave this alone". */
    if (f->type == FLD_STR && value[0] == '\0') return;
    if (!field_parse(f, c, value)) {
      if (f->hint != NULL)
        g_warning("pt: config line %d: bad %s '%s' (%s)", lineno, f->key,
                  value, f->hint);
      else
        g_warning("pt: config line %d: bad %s '%s'", lineno, f->key, value);
    }
    return;
  }
  if (g_str_has_prefix(key, "app-") && key[4] != '\0')
    g_hash_table_insert(c->app_overrides, g_strdup(key + 4), g_strdup(value));
  /* Unknown keys: ignored on read, preserved by rewrite. */
}

guint pt_config_n_binding_lines(const PtConfig *cfg) {
  return cfg->binding_lines->len;
}

const char *pt_config_binding_line(const PtConfig *cfg, guint i) {
  return ((const PtBindingSpec *)g_ptr_array_index(cfg->binding_lines, i))
      ->text;
}

int pt_config_binding_line_no(const PtConfig *cfg, guint i) {
  return ((const PtBindingSpec *)g_ptr_array_index(cfg->binding_lines, i))
      ->line_no;
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
/* The keys the app writes back are exactly the field table's rows. */
#define PT_MANAGED_KEYS ((int)G_N_ELEMENTS(config_fields))

char *pt_config_rewrite(const char *old_text, const PtConfig *c) {
  char *values[PT_MANAGED_KEYS];
  gboolean written[PT_MANAGED_KEYS];
  for (int i = 0; i < PT_MANAGED_KEYS; i++) {
    values[i] = field_value(&config_fields[i], c);
    written[i] = FALSE;
  }

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
          if (g_strcmp0(key, config_fields[k].key) == 0) {
            g_string_append_printf(out, "%s = %s\n",
                                   config_fields[k].key, values[k]);
            written[k] = TRUE;
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
    if (!written[k])
      g_string_append_printf(out, "%s = %s\n", config_fields[k].key, values[k]);
    g_free(values[k]);
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
