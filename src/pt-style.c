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

void pt_style_apply(const PtResolvedTheme *rt, const PtConfig *cfg) {
  g_return_if_fail(var_provider != NULL);
  char *css = pt_style_css(rt, cfg);
  /* Reloading the provider restyles every widget on the display, and the
   * settings dialog re-applies at key-repeat rate: when the generated text is
   * exactly what is already loaded, loading it again buys nothing. Building
   * the string just to compare it is the cheap side of that trade. */
  static char *last_css;
  if (g_strcmp0(last_css, css) == 0) {
    g_free(css);
    return;
  }
  gtk_css_provider_load_from_string(var_provider, css);
  g_free(last_css);
  last_css = css;   /* keeps the text the provider is holding */
}
