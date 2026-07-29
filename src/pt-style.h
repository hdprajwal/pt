#pragma once
#include <gtk/gtk.h>
#include "pt-config.h"
#include "pt-style-css.h"   /* pt_style_css(): the text this applies */
#include "pt-theme.h"

/* Idempotent. Adds the provider that carries the theme's CSS variables, at
 * USER priority; the static stylesheet's provider is installed in main.c. */
void pt_style_init(GdkDisplay *display);
/* Loads pt_style_css(rt, cfg) into that provider, skipping the reload when the
 * text is what is already loaded. */
void pt_style_apply(const PtResolvedTheme *rt, const PtConfig *cfg);
