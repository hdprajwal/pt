#pragma once
#include <gtk/gtk.h>
#include "pt-config.h"
#include "pt-theme.h"

void pt_style_init(GdkDisplay *display);  /* idempotent; adds both providers */
void pt_style_apply(const PtResolvedTheme *rt, const PtConfig *cfg);
