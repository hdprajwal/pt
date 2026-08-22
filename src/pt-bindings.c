#include "pt-bindings.h"
#include <string.h>

/* ---------- action-name space ----------
 * One row per (id, arg) the config may name. The arg values are the ids'
 * own arguments: a project/tab index, PtSplitKind (included here — it is a
 * pure type), or the PtPaneDirection order from pt-pane-grid.h, spelled as
 * literals because that header drags GTK in and this module must not know
 * it. pt-window.c static-asserts the two orders stay in step. */
static const PtBindingAction actions[] = {
  { "switch-project-1", PT_ACTION_SWITCH_PROJECT, 0 },
  { "switch-project-2", PT_ACTION_SWITCH_PROJECT, 1 },
  { "switch-project-3", PT_ACTION_SWITCH_PROJECT, 2 },
  { "switch-project-4", PT_ACTION_SWITCH_PROJECT, 3 },
  { "switch-project-5", PT_ACTION_SWITCH_PROJECT, 4 },
  { "switch-project-6", PT_ACTION_SWITCH_PROJECT, 5 },
  { "switch-project-7", PT_ACTION_SWITCH_PROJECT, 6 },
  { "switch-project-8", PT_ACTION_SWITCH_PROJECT, 7 },
  { "switch-project-9", PT_ACTION_SWITCH_PROJECT, 8 },
  { "switch-tab-1",     PT_ACTION_SWITCH_TAB,     0 },
  { "switch-tab-2",     PT_ACTION_SWITCH_TAB,     1 },
  { "switch-tab-3",     PT_ACTION_SWITCH_TAB,     2 },
  { "switch-tab-4",     PT_ACTION_SWITCH_TAB,     3 },
  { "switch-tab-5",     PT_ACTION_SWITCH_TAB,     4 },
  { "switch-tab-6",     PT_ACTION_SWITCH_TAB,     5 },
  { "switch-tab-7",     PT_ACTION_SWITCH_TAB,     6 },
  { "switch-tab-8",     PT_ACTION_SWITCH_TAB,     7 },
  { "switch-tab-9",     PT_ACTION_SWITCH_TAB,     8 },
  { "new-tab",          PT_ACTION_NEW_TAB,          0 },
  { "add-project",      PT_ACTION_ADD_PROJECT,      0 },
  { "toggle-sidebar",   PT_ACTION_TOGGLE_SIDEBAR,   0 },
  { "toggle-infopanel", PT_ACTION_TOGGLE_INFOPANEL, 0 },
  { "next-tab",         PT_ACTION_NEXT_TAB,         0 },
  { "prev-tab",         PT_ACTION_PREV_TAB,         0 },
  { "next-project",     PT_ACTION_NEXT_PROJECT,     0 },
  { "prev-project",     PT_ACTION_PREV_PROJECT,     0 },
  { "split-h",          PT_ACTION_SPLIT,            PT_SPLIT_H },
  { "split-v",          PT_ACTION_SPLIT,            PT_SPLIT_V },
  { "close-pane",       PT_ACTION_CLOSE_PANE,       0 },
  /* PtPaneDirection: left, right, up, down — see the static asserts. */
  { "focus-left",       PT_ACTION_FOCUS_DIRECTION,  0 },
  { "focus-right",      PT_ACTION_FOCUS_DIRECTION,  1 },
  { "focus-up",         PT_ACTION_FOCUS_DIRECTION,  2 },
  { "focus-down",       PT_ACTION_FOCUS_DIRECTION,  3 },
  { "focus-next",       PT_ACTION_FOCUS_NEXT,       0 },
  { "focus-prev",       PT_ACTION_FOCUS_PREV,       0 },
  { "paste",            PT_ACTION_PASTE,            0 },
  { "copy",             PT_ACTION_COPY,             0 },
  { "font-zoom-in",     PT_ACTION_ZOOM,            +1 },
  { "font-zoom-out",    PT_ACTION_ZOOM,            -1 },
  { "font-zoom-reset",  PT_ACTION_ZOOM,             0 },
  { "pane-zoom",        PT_ACTION_ZOOM_PANE,        0 },
};

gboolean pt_bindings_action_lookup(const char *name, PtActionId *id,
                                   int *arg) {
  if (name == NULL || name[0] == '\0') return FALSE;
  for (gsize i = 0; i < G_N_ELEMENTS(actions); i++)
    if (g_ascii_strcasecmp(name, actions[i].name) == 0) {
      if (id != NULL) *id = actions[i].id;
      if (arg != NULL) *arg = actions[i].arg;
      return TRUE;
    }
  return FALSE;
}

/* ---------- accelerator grammar ----------
 * Lowercase modifiers ctrl/shift/alt/super joined by `+`, ending in a named
 * key. Punctuation keys use names only (`equal`, never `=`): the config
 * format splits on nothing inside a value, but `+` is itself a key and a
 * bare `=` reads as noise. Output is GTK's accelerator spelling so the
 * window layer parses it without a second table. */

/* Config spelling -> GTK keysym name. */
static const struct { const char *from, *to; } key_names[] = {
  { "enter",      "Return"      },
  { "tab",        "Tab"         },
  { "escape",     "Escape"      },
  { "space",      "space"       },
  { "up",         "Up"          },
  { "down",        "Down"       },
  { "left",       "Left"        },
  { "right",      "Right"       },
  { "pgup",       "Page_Up"     },
  { "pgdn",       "Page_Down"   },
  { "home",       "Home"        },
  { "end",        "End"         },
  { "delete",     "Delete"      },
  { "backspace",  "BackSpace"   },
  { "equal",      "equal"       },
  { "plus",       "plus"        },
  { "minus",      "minus"       },
  { "comma",      "comma"       },
  { "period",     "period"      },
  { "slash",      "slash"       },
  { "backslash",  "backslash"   },
  { "semicolon",  "semicolon"   },
  { "quote",      "apostrophe"  },
  { "bracketleft",  "bracketleft"  },
  { "bracketright", "bracketright" },
};

/* Appends GTK's spelling of one lowercase key token to out. FALSE when the
 * token names no key this grammar knows. */
static gboolean key_append(const char *k, GString *out) {
  gsize len = strlen(k);
  if (len == 1 && ((k[0] >= 'a' && k[0] <= 'z') ||
                   (k[0] >= '0' && k[0] <= '9'))) {
    g_string_append(out, k);           /* letters and digits pass through */
    return TRUE;
  }
  if (k[0] == 'f' && len >= 2) {       /* f1..f24 */
    char *end = NULL;
    long n = strtol(k + 1, &end, 10);
    if (end != NULL && *end == '\0' && n >= 1 && n <= 24) {
      g_string_append_printf(out, "F%ld", n);
      return TRUE;
    }
  }
  for (gsize i = 0; i < G_N_ELEMENTS(key_names); i++)
    if (strcmp(k, key_names[i].from) == 0) {
      g_string_append(out, key_names[i].to);
      return TRUE;
    }
  return FALSE;
}

/* Builds the canonical accel for one `<accel>` word, or returns FALSE. */
static gboolean accel_build(const char *word, GString *out) {
  char *low = g_ascii_strdown(word, -1);
  gchar **parts = g_strsplit(low, "+", -1);
  guint n = g_strv_length(parts);
  gboolean ok = n > 0 && parts[n - 1][0] != '\0';
  gboolean m_ctrl = FALSE, m_shift = FALSE, m_alt = FALSE, m_super = FALSE;
  if (ok)
    for (guint i = 0; ok && i < n - 1; i++) {
      const char *m = parts[i];
      gboolean *slot = strcmp(m, "ctrl") == 0   ? &m_ctrl
                     : strcmp(m, "shift") == 0 ? &m_shift
                     : strcmp(m, "alt") == 0   ? &m_alt
                     : strcmp(m, "super") == 0 ? &m_super
                                               : NULL;
      /* A repeated modifier is a typo, and an empty piece means a literal
       * `+` tried to travel as a separator. */
      if (slot == NULL || *slot || m[0] == '\0') ok = FALSE;
      else *slot = TRUE;
    }
  if (ok) {
    if (m_ctrl)  g_string_append(out, "<Control>");
    if (m_shift) g_string_append(out, "<Shift>");
    if (m_alt)   g_string_append(out, "<Alt>");
    if (m_super) g_string_append(out, "<Super>");
    ok = key_append(parts[n - 1], out);
  }
  g_strfreev(parts);
  g_free(low);
  return ok;
}

/* ---------- line parsing ---------- */

static void binding_line_free(gpointer p) {
  PtBindingLine *b = p;
  g_free(b->accel);
  g_free(b->action);
  g_free(b);
}

/* A later line replaces an earlier one for the same accelerator, whatever
 * each did: bind after unbind binds it, unbind after bind removes it. */
static void drop_accel(GPtrArray *out, const char *accel) {
  for (guint i = out->len; i-- > 0;) {
    const PtBindingLine *b = g_ptr_array_index(out, i);
    if (strcmp(b->accel, accel) == 0) g_ptr_array_remove_index(out, i);
  }
}

static void parse_line(const char *line, int line_no, GPtrArray *out) {
  char *strip = g_strstrip(g_strdup(line));
  gchar **raw = g_strsplit(strip, " ", -1);
  /* Collapse runs of spaces into real words. */
  gchar **words = g_new0(gchar *, g_strv_length(raw) + 1);
  guint n = 0;
  for (guint i = 0; raw[i] != NULL; i++)
    if (raw[i][0] != '\0') words[n++] = raw[i];

  /* The verb, like everything else on the line, reads either case. */
  char *verb_buf = n > 0 ? g_ascii_strdown(words[0], -1) : NULL;
  const char *verb = verb_buf != NULL ? verb_buf : "";
  if (strcmp(verb, "bind") != 0 && strcmp(verb, "unbind") != 0) {
    g_warning("pt: config line %d: expected 'bind' or 'unbind' — skipped",
              line_no);
    goto done;
  }
  if (n < 2) {
    g_warning("pt: config line %d: %s needs an accelerator — skipped",
              line_no, verb);
    goto done;
  }
  if (n > (strcmp(verb, "bind") == 0 ? 3 : 2)) {
    g_warning("pt: config line %d: too many fields — skipped", line_no);
    goto done;
  }
  if (strcmp(verb, "unbind") == 0 && n == 3) {
    g_warning("pt: config line %d: unbind takes no action — skipped",
              line_no);
    goto done;
  }

  GString *canon = g_string_new(NULL);
  if (!accel_build(words[1], canon)) {
    g_warning("pt: config line %d: bad accelerator '%s' — skipped", line_no,
              words[1]);
    g_string_free(canon, TRUE);
    goto done;
  }

  PtBindingLine *b = g_new0(PtBindingLine, 1);
  b->accel = g_string_free(canon, FALSE);
  b->is_unbind = strcmp(verb, "unbind") == 0;
  b->line_no = line_no;
  if (!b->is_unbind) {
    char *action = g_ascii_strdown(words[2], -1);
    if (!pt_bindings_action_lookup(action, NULL, NULL)) {
      g_warning("pt: config line %d: unknown action '%s' — skipped", line_no,
                words[2]);
      binding_line_free(b);
      g_free(action);
      goto done;
    }
    b->action = action;
  }
  drop_accel(out, b->accel);
  g_ptr_array_add(out, b);

done:
  g_free(verb_buf);
  g_free(words);   /* words borrows raw's strings */
  g_strfreev(raw);
  g_free(strip);
}

GPtrArray *pt_bindings_parse(const char *const *lines, const int *line_nos,
                             GError **error) {
  if (error != NULL) *error = NULL;
  GPtrArray *out = g_ptr_array_new_with_free_func(binding_line_free);
  if (lines == NULL) return out;
  for (guint i = 0; lines[i] != NULL; i++)
    parse_line(lines[i], line_nos != NULL ? line_nos[i] : (int)i + 1, out);
  return out;
}

void pt_bindings_free(GPtrArray *bindings) {
  if (bindings != NULL) g_ptr_array_unref(bindings);
}
