#include "pt-theme.h"
#include "pt-config.h"   /* pt_kv_parse */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- color ---- */
gboolean pt_color_parse(const char *s, PtColor *out) {
  if (s == NULL) return FALSE;
  if (s[0] == '#' && strlen(s) == 7) {
    /* Every one of the six must be a hex digit: strtol() would otherwise take
     * a sign or leading space and hand back an unrelated color — "#-00001"
     * sign-extends into pure white, "#+12345" reads as "#012345". */
    for (int i = 1; i <= 6; i++)
      if (!g_ascii_isxdigit(s[i])) return FALSE;
    long v = strtol(s + 1, NULL, 16);
    out->r = (v >> 16) & 0xff;
    out->g = (v >> 8) & 0xff;
    out->b = v & 0xff;
    out->a = 1.0;
    return TRUE;
  }
  if (g_str_has_prefix(s, "rgba(")) {
    int r, g, b;
    char abuf[32];
    if (sscanf(s, "rgba( %d , %d , %d , %31[^)])", &r, &g, &b, abuf) == 4 ||
        sscanf(s, "rgba(%d,%d,%d,%31[^)])", &r, &g, &b, abuf) == 4) {
      char *end = NULL;
      double a = g_ascii_strtod(g_strstrip(abuf), &end);
      if (end == abuf) return FALSE;
      if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) return FALSE;
      out->r = (guint8)r; out->g = (guint8)g; out->b = (guint8)b;
      out->a = CLAMP(a, 0.0, 1.0);
      return TRUE;
    }
  }
  return FALSE;
}

char *pt_color_to_css(const PtColor *c) {
  if (c->a >= 1.0)
    return g_strdup_printf("#%02x%02x%02x", c->r, c->g, c->b);
  char abuf[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(abuf, sizeof abuf, "%g", c->a);
  return g_strdup_printf("rgba(%d,%d,%d,%s)", c->r, c->g, c->b, abuf);
}

/* ---- token table ---- */
static const char *token_names[PT_TOK_COUNT] = {
  "background", "surface", "surface-alt", "panel",
  "terminal-clear", "scrim",
  "border", "border-strong", "hover", "active",
  "active-strong", "tag-bg", "track", "slider",
  "slider-hover", "field-bg",
  "text", "text-muted", "text-mid", "text-faint",
  "text-dim", "text-ghost",
  "ok", "ok-muted", "err", "warn", "focus-ring",
  "accent-0", "accent-1", "accent-2", "accent-3",
  "accent-4", "accent-5",
};

const char *pt_theme_token_name(PtTokenId id) { return token_names[id]; }

/* ---- builtin ---- */
/* Pins every token so the default look never depends on derivation. */
const char *pt_theme_builtin_pt_dark(void) {
  return
    "# pt-dark — built-in default\n"
    "background = #0b0d10\n"
    "foreground = #d6dae0\n"
    "cursor-color = #d6dae0\n"
    "selection-background = #264f38\n"
    "palette = 1=#f2777a\n"
    "palette = 2=#6ee7a0\n"
    "palette = 3=#f2b25c\n"
    "palette = 9=#f2777a\n"
    "palette = 10=#6ee7a0\n"
    "palette = 11=#f2b25c\n"
    "app-background = #0e1013\n"
    "app-surface = #12151a\n"
    "app-surface-alt = #101317\n"
    "app-panel = #15181d\n"
    "app-terminal-clear = #0b0d10\n"
    "app-scrim = rgba(6,8,10,0.62)\n"
    "app-border = rgba(255,255,255,0.1)\n"
    "app-border-strong = rgba(255,255,255,0.1)\n"
    "app-hover = rgba(255,255,255,0.045)\n"
    "app-active = rgba(255,255,255,0.07)\n"
    "app-active-strong = rgba(255,255,255,0.08)\n"
    "app-tag-bg = rgba(255,255,255,0.05)\n"
    "app-field-bg = rgba(255,255,255,0.04)\n"
    "app-track = rgba(255,255,255,0.09)\n"
    "app-slider = rgba(255,255,255,0.12)\n"
    "app-slider-hover = rgba(255,255,255,0.22)\n"
    "app-text = #e6e8ea\n"
    "app-text-muted = #8a9199\n"
    "app-text-mid = #727a85\n"
    "app-text-faint = #5c646f\n"
    "app-text-dim = #4e5661\n"
    "app-text-ghost = #3f4650\n"
    "app-ok = #6ee7a0\n"
    "app-ok-muted = #6b747f\n"
    "app-err = #f2777a\n"
    "app-warn = #f2b25c\n"
    "app-focus-ring = #2f4f3a\n"
    "app-accent-0 = #6ee7a0\n"
    "app-accent-1 = #8ab4f8\n"
    "app-accent-2 = #f2b25c\n"
    "app-accent-3 = #c99bf0\n"
    "app-accent-4 = #5ed3c4\n"
    "app-accent-5 = #e0849b\n";
}

/* ---- parse ---- */
typedef struct { PtTheme *t; } ParseCtx;

static void theme_kv(const char *key, const char *value, int lineno,
                     gpointer user) {
  PtTheme *t = ((ParseCtx *)user)->t;
  if (value == NULL) {
    g_warning("pt: theme line %d: no '=' — skipped", lineno);
    return;
  }
  PtColor c;
  if (g_strcmp0(key, "background") == 0) {
    if (pt_color_parse(value, &c)) t->background = c;
  } else if (g_strcmp0(key, "foreground") == 0) {
    if (pt_color_parse(value, &c)) t->foreground = c;
  } else if (g_strcmp0(key, "cursor-color") == 0) {
    if (pt_color_parse(value, &c)) { t->cursor = c; t->cursor_set = TRUE; }
  } else if (g_strcmp0(key, "selection-background") == 0) {
    if (pt_color_parse(value, &c)) t->selection_bg = c;
  } else if (g_strcmp0(key, "palette") == 0) {
    /* "N=#rrggbb" */
    char *eq = strchr(value, '=');
    if (eq == NULL) { g_warning("pt: theme line %d: bad palette", lineno); return; }
    char *nstr = g_strstrip(g_strndup(value, (gsize)(eq - value)));
    char *nend = NULL;
    gint64 n = g_ascii_strtoll(nstr, &nend, 10);
    /* The whole trimmed index must be digits. atoi() reads "x" and "" as 0 and
     * would silently repaint ANSI 0; strtoll alone would still take a sign. */
    gboolean index_ok = g_ascii_isdigit(nstr[0]) && *nend == '\0';
    g_free(nstr);
    char *cstr = g_strstrip(g_strdup(eq + 1));
    if (index_ok && n >= 0 && n < 16 && pt_color_parse(cstr, &c)) {
      t->palette[n] = c;
      t->palette_set[n] = TRUE;
    } else {
      g_warning("pt: theme line %d: bad palette entry '%s'", lineno, value);
    }
    g_free(cstr);
  } else if (g_str_has_prefix(key, "app-") && key[4] != '\0') {
    g_hash_table_insert(t->app_overrides, g_strdup(key + 4), g_strdup(value));
  }
}

PtTheme *pt_theme_parse(const char *text) {
  PtTheme *t = g_new0(PtTheme, 1);
  t->app_overrides = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, g_free);
  /* pt-dark terminal values are the fallback for missing keys. */
  t->background   = (PtColor){0x0b, 0x0d, 0x10, 1.0};
  t->foreground   = (PtColor){0xd6, 0xda, 0xe0, 1.0};
  t->selection_bg = (PtColor){0x26, 0x4f, 0x38, 1.0};
  ParseCtx ctx = { t };
  pt_kv_parse(text, theme_kv, &ctx);
  if (!t->cursor_set) t->cursor = t->foreground;
  return t;
}

void pt_theme_free(PtTheme *t) {
  if (t == NULL) return;
  g_hash_table_unref(t->app_overrides);
  g_free(t);
}

/* ---- derivation ---- */
static double srgb_lum(const PtColor *c) {   /* rough perceived lightness */
  return (0.299 * c->r + 0.587 * c->g + 0.114 * c->b) / 255.0;
}

/* The one definition of "is this theme dark", used both for deriving the chrome
 * and for what pt tells programs. ghostty runs the same formula
 * (terminal/color.zig:484) against the same 0.5, but branches the other way
 * (`lum > 0.5` → light, application.zig:1370), so the two disagree at exactly
 * 0.5 and nowhere else. pt's direction is kept: it has been picking the chrome
 * since before anyone asked it about color schemes. */
static gboolean bg_is_dark(const PtColor *bg) { return srgb_lum(bg) < 0.5; }

static PtColor shift_l(const PtColor *c, double delta) {
  /* Move toward white (delta>0) or black (delta<0) in linear-ish space. */
  PtColor out = *c;
  double t = fabs(delta);
  guint8 target = delta > 0 ? 255 : 0;
  out.r = (guint8)round(c->r + (target - c->r) * t);
  out.g = (guint8)round(c->g + (target - c->g) * t);
  out.b = (guint8)round(c->b + (target - c->b) * t);
  out.a = 1.0;
  return out;
}

static PtColor mix(const PtColor *a, const PtColor *b, double t) {
  return (PtColor){
    (guint8)round(a->r + (b->r - a->r) * t),
    (guint8)round(a->g + (b->g - a->g) * t),
    (guint8)round(a->b + (b->b - a->b) * t),
    1.0,
  };
}

static PtColor with_alpha(const PtColor *c, double a) {
  PtColor out = *c;
  out.a = a;
  return out;
}

static PtColor pal_or(const PtTheme *t, int slot, PtColor fallback) {
  return t->palette_set[slot] ? t->palette[slot] : fallback;
}

void pt_theme_resolve(const PtTheme *t, GHashTable *config_overrides,
                      PtResolvedTheme *out) {
  memset(out, 0, sizeof *out);
  out->term = *t;
  out->term.app_overrides = NULL;  /* not owned by the resolved copy */

  const PtColor bg = t->background, fg = t->foreground;
  gboolean dark = bg_is_dark(&bg);
  out->dark = dark;
  double dir = dark ? 1.0 : -1.0;
  PtColor *tok = out->tokens;

  tok[PT_TOK_BACKGROUND]     = shift_l(&bg, dir * 0.015);
  tok[PT_TOK_SURFACE]        = shift_l(&bg, dir * 0.035);
  tok[PT_TOK_SURFACE_ALT]    = shift_l(&bg, dir * 0.025);
  tok[PT_TOK_PANEL]          = shift_l(&bg, dir * 0.050);
  tok[PT_TOK_TERMINAL_CLEAR] = bg;
  tok[PT_TOK_SCRIM]          = with_alpha(&(PtColor){bg.r, bg.g, bg.b, 1}, 0.62);
  PtColor white = {255, 255, 255, 1.0}, black = {0, 0, 0, 1.0};
  PtColor edge = dark ? white : black;
  tok[PT_TOK_BORDER]        = with_alpha(&edge, 0.06);
  tok[PT_TOK_BORDER_STRONG] = with_alpha(&edge, 0.1);
  tok[PT_TOK_HOVER]         = with_alpha(&edge, 0.045);
  tok[PT_TOK_ACTIVE]        = with_alpha(&edge, 0.07);
  tok[PT_TOK_ACTIVE_STRONG] = with_alpha(&edge, 0.08);
  tok[PT_TOK_TAG_BG]        = with_alpha(&edge, 0.05);
  tok[PT_TOK_TRACK]         = with_alpha(&edge, 0.09);
  tok[PT_TOK_SLIDER]        = with_alpha(&edge, 0.12);
  tok[PT_TOK_SLIDER_HOVER]  = with_alpha(&edge, 0.22);
  tok[PT_TOK_FIELD_BG]      = with_alpha(&edge, 0.04);
  tok[PT_TOK_TEXT]       = fg;
  tok[PT_TOK_TEXT_MUTED] = mix(&fg, &bg, 0.38);
  tok[PT_TOK_TEXT_MID]   = mix(&fg, &bg, 0.46);
  tok[PT_TOK_TEXT_FAINT] = mix(&fg, &bg, 0.56);
  tok[PT_TOK_TEXT_DIM]   = mix(&fg, &bg, 0.62);
  tok[PT_TOK_TEXT_GHOST] = mix(&fg, &bg, 0.70);
  PtColor def_ok   = {0x6e, 0xe7, 0xa0, 1.0};
  PtColor def_err  = {0xf2, 0x77, 0x7a, 1.0};
  PtColor def_warn = {0xf2, 0xb2, 0x5c, 1.0};
  PtColor def_a1 = {0x8a, 0xb4, 0xf8, 1.0}, def_a3 = {0xc9, 0x9b, 0xf0, 1.0},
          def_a4 = {0x5e, 0xd3, 0xc4, 1.0}, def_a5 = {0xe0, 0x84, 0x9b, 1.0};
  tok[PT_TOK_OK]       = pal_or(t, 2, def_ok);
  tok[PT_TOK_OK_MUTED] = mix(&fg, &bg, 0.50);
  tok[PT_TOK_ERR]      = pal_or(t, 1, def_err);
  tok[PT_TOK_WARN]     = pal_or(t, 3, def_warn);
  /* The cursor color is the closest thing a terminal theme has to a brand
   * accent (it falls back to the foreground when unset), so the focus ring
   * follows it instead of hard-wiring ANSI green. */
  tok[PT_TOK_FOCUS_RING] = mix(&t->cursor, &bg, 0.55);
  tok[PT_TOK_ACCENT_0] = pal_or(t, 2, def_ok);
  tok[PT_TOK_ACCENT_1] = pal_or(t, 4, def_a1);
  tok[PT_TOK_ACCENT_2] = pal_or(t, 3, def_warn);
  tok[PT_TOK_ACCENT_3] = pal_or(t, 5, def_a3);
  tok[PT_TOK_ACCENT_4] = pal_or(t, 6, def_a4);
  tok[PT_TOK_ACCENT_5] = pal_or(t, 1, def_a5);

  /* overrides: theme file first, then config on top */
  GHashTable *layers[2] = { t->app_overrides, config_overrides };
  for (int layer = 0; layer < 2; layer++) {
    if (layers[layer] == NULL) continue;
    for (int i = 0; i < PT_TOK_COUNT; i++) {
      const char *v = g_hash_table_lookup(layers[layer], token_names[i]);
      PtColor c;
      if (v != NULL && pt_color_parse(v, &c)) tok[i] = c;
      else if (v != NULL)
        g_warning("pt: bad color for app-%s: '%s'", token_names[i], v);
    }
  }
}

/* ---- discovery ---- */
char *pt_theme_dir(void) {
  return g_build_filename(g_get_user_config_dir(), "pt", "themes", NULL);
}

static const char *builtin_names[] = { "pt-dark", NULL };

char **pt_theme_list_names(const char *dir) {
  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
  for (int i = 0; builtin_names[i] != NULL; i++)
    g_ptr_array_add(arr, g_strdup(builtin_names[i]));
  GDir *d = g_dir_open(dir, 0, NULL);
  if (d != NULL) {
    const char *name;
    while ((name = g_dir_read_name(d)) != NULL) {
      gboolean dup = FALSE;
      for (guint j = 0; j < arr->len && !dup; j++)
        dup = g_strcmp0(g_ptr_array_index(arr, j), name) == 0;
      if (!dup) g_ptr_array_add(arr, g_strdup(name));
    }
    g_dir_close(d);
  }
  g_ptr_array_sort_values(arr, (GCompareFunc)g_strcmp0);
  g_ptr_array_add(arr, NULL);
  return (char **)g_ptr_array_free(arr, FALSE);
}

char *pt_theme_load_text(const char *dir, const char *name) {
  char *path = g_build_filename(dir, name, NULL);
  char *text = NULL;
  gboolean ok = g_file_get_contents(path, &text, NULL, NULL);
  g_free(path);
  if (ok) return text;
  if (g_strcmp0(name, "pt-dark") == 0)
    return g_strdup(pt_theme_builtin_pt_dark());
  return NULL;
}

gboolean pt_theme_is_dark(const char *dir, const char *name) {
  char *text = pt_theme_load_text(dir, name);
  if (text == NULL) return TRUE;   /* unknown name: pt's own theme is dark */
  PtTheme *t = pt_theme_parse(text);
  /* Straight off the parsed background rather than through pt_theme_resolve():
   * the answer does not depend on the chrome tokens or on any override, and a
   * caller classifying every installed theme should not pay to derive 33 colors
   * per name. App overrides cannot move it either — they repaint pt's chrome,
   * not the terminal background this reads. */
  gboolean dark = bg_is_dark(&t->background);
  pt_theme_free(t);
  g_free(text);
  return dark;
}

char **pt_theme_filter_appearance(const char *const *names, gboolean dark,
                                  PtThemeDarkFn classify, gpointer user) {
  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
  /* One classify() call per name, in order: the caller pays for reading and
   * parsing each theme file once, and the two subsets it builds keep the same
   * order as the list it started from. */
  if (names != NULL && classify != NULL) {
    for (int i = 0; names[i] != NULL; i++) {
      /* Compared as truths, not as ints: a classifier is free to answer with
       * any non-zero value for dark. */
      if (!classify(names[i], user) == !dark)
        g_ptr_array_add(arr, g_strdup(names[i]));
    }
  }
  g_ptr_array_add(arr, NULL);
  return (char **)g_ptr_array_free(arr, FALSE);
}
