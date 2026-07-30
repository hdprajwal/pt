#pragma once
#include <glib.h>
#include "pt-config.h"
#include "pt-theme.h"

/* The `:root` block that carries a resolved theme and the UI font settings to
 * src/style.css as CSS custom properties: one --pt-<token> per chrome token,
 * the per-accent glow/chip and ok-glow rgba() derivations, the two font
 * families, and the nine --pt-fs-* size roles as ratios of ui-font-size.
 * Caller frees.
 * GTK-free and pure so the text can be tested on its own; pt_style_apply() is
 * the few lines that load it into the display's provider. It skips the load
 * when the text has not changed, comparing with strcmp — so the output must
 * stay a deterministic, byte-for-byte function of these two inputs. */
char *pt_style_css(const PtResolvedTheme *rt, const PtConfig *cfg);
