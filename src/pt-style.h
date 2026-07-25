#pragma once
#include <gtk/gtk.h>
#include "pt-config.h"
#include "pt-theme.h"

/* Idempotent. Adds the provider that carries the theme's CSS variables, at
 * USER priority; the static stylesheet's provider is installed in main.c. */
void pt_style_init(GdkDisplay *display);
void pt_style_apply(const PtResolvedTheme *rt, const PtConfig *cfg);
