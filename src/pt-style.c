#include "pt-style.h"

static GtkCssProvider *var_provider;

void pt_style_init(GdkDisplay *display) {
  if (var_provider != NULL) return;
  var_provider = gtk_css_provider_new();
  /* One above the base stylesheet (USER + 1 in main.c), keeping the token
   * provider on top and both out of reach of user gtk.css themes. */
  gtk_style_context_add_provider_for_display(
      display, GTK_STYLE_PROVIDER(var_provider),
      GTK_STYLE_PROVIDER_PRIORITY_USER + 2);
}

/* Font size roles, as ratios of ui-font-size (base 12.5 in the shipped
 * design). Emitted with 0.1px precision. */
static const struct { const char *var; double ratio; } font_roles[] = {
  { "--pt-fs-lg",   13.0 / 12.5 },
  { "--pt-fs-base", 1.0 },
  { "--pt-fs-md",   12.0 / 12.5 },
  { "--pt-fs-sm2",  11.5 / 12.5 },
  { "--pt-fs-sm",   11.0 / 12.5 },
  { "--pt-fs-xs",   10.5 / 12.5 },
  { "--pt-fs-2xs",   9.5 / 12.5 },
  { "--pt-fs-icon", 16.0 / 12.5 },
  { "--pt-fs-plus", 14.0 / 12.5 },
};

void pt_style_apply(const PtResolvedTheme *rt, const PtConfig *cfg) {
  g_return_if_fail(var_provider != NULL);
  GString *css = g_string_new(":root {\n");
  for (int i = 0; i < PT_TOK_COUNT; i++) {
    char *v = pt_color_to_css(&rt->tokens[i]);
    g_string_append_printf(css, "  --pt-%s: %s;\n",
                           pt_theme_token_name((PtTokenId)i), v);
    g_free(v);
  }
  /* accent glows (dot ring .13, chip bg .10) + ok glow (.18) */
  for (int a = 0; a < 6; a++) {
    const PtColor *c = &rt->tokens[PT_TOK_ACCENT_0 + a];
    g_string_append_printf(css,
        "  --pt-accent-%d-glow: rgba(%d,%d,%d,0.13);\n"
        "  --pt-accent-%d-chip: rgba(%d,%d,%d,0.10);\n",
        a, c->r, c->g, c->b, a, c->r, c->g, c->b);
  }
  const PtColor *ok = &rt->tokens[PT_TOK_OK];
  g_string_append_printf(css, "  --pt-ok-glow: rgba(%d,%d,%d,0.18);\n",
                         ok->r, ok->g, ok->b);
  /* fonts */
  g_string_append_printf(css, "  --pt-font-sans: \"%s\", sans-serif;\n",
                         cfg->ui_font_family);
  g_string_append_printf(css, "  --pt-font-mono: \"%s\", monospace;\n",
                         cfg->font_family);
  /* g_ascii_formatd, not %.1f: printf's decimal separator follows LC_NUMERIC,
   * and a comma here would make every font-size a CSS parse error. */
  for (gsize i = 0; i < G_N_ELEMENTS(font_roles); i++) {
    char nbuf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(nbuf, sizeof nbuf, "%.1f",
                    cfg->ui_font_size * font_roles[i].ratio);
    g_string_append_printf(css, "  %s: %spx;\n", font_roles[i].var, nbuf);
  }
  g_string_append(css, "}\n");
  /* Reloading the provider restyles every widget on the display, and the
   * settings dialog re-applies at key-repeat rate: when the generated text is
   * exactly what is already loaded, loading it again buys nothing. Building
   * the string just to compare it is the cheap side of that trade. */
  static char *last_css;
  if (g_strcmp0(last_css, css->str) == 0) {
    g_string_free(css, TRUE);
    return;
  }
  gtk_css_provider_load_from_string(var_provider, css->str);
  g_free(last_css);
  last_css = g_string_free(css, FALSE);
}
