#pragma once
#include <glib.h>

typedef struct { guint8 r, g, b; double a; } PtColor;  /* a in 0..1 */

/* Chrome token ids — ordering is ABI for pt_theme_token_name(). */
typedef enum {
  PT_TOK_BACKGROUND, PT_TOK_SURFACE, PT_TOK_SURFACE_ALT, PT_TOK_PANEL,
  PT_TOK_TERMINAL_CLEAR, PT_TOK_SCRIM,
  PT_TOK_BORDER, PT_TOK_BORDER_STRONG, PT_TOK_HOVER, PT_TOK_ACTIVE,
  PT_TOK_ACTIVE_STRONG, PT_TOK_TAG_BG, PT_TOK_TRACK, PT_TOK_SLIDER,
  PT_TOK_SLIDER_HOVER,
  PT_TOK_TEXT, PT_TOK_TEXT_MUTED, PT_TOK_TEXT_MID, PT_TOK_TEXT_FAINT,
  PT_TOK_TEXT_DIM, PT_TOK_TEXT_GHOST,
  PT_TOK_OK, PT_TOK_OK_MUTED, PT_TOK_ERR, PT_TOK_WARN, PT_TOK_FOCUS_RING,
  PT_TOK_ACCENT_0, PT_TOK_ACCENT_1, PT_TOK_ACCENT_2, PT_TOK_ACCENT_3,
  PT_TOK_ACCENT_4, PT_TOK_ACCENT_5,
  PT_TOK_COUNT
} PtTokenId;

typedef struct {
  PtColor background, foreground, cursor, selection_bg;
  PtColor palette[16];
  gboolean palette_set[16];     /* slots the theme actually pins */
  gboolean cursor_set;
  GHashTable *app_overrides;    /* same shape as PtConfig.app_overrides */
} PtTheme;

typedef struct {
  PtTheme term;                 /* resolved terminal colors */
  PtColor tokens[PT_TOK_COUNT]; /* resolved chrome tokens */
} PtResolvedTheme;

gboolean    pt_color_parse(const char *s, PtColor *out); /* #rrggbb | rgba(r,g,b,a) */
char       *pt_color_to_css(const PtColor *c);           /* "#rrggbb" or "rgba(...)" */
const char *pt_theme_token_name(PtTokenId id);           /* "background", "accent-0", ... */
const char *pt_theme_builtin_pt_dark(void);              /* full theme text */
PtTheme    *pt_theme_parse(const char *text);            /* missing keys <- pt-dark values */
void        pt_theme_free(PtTheme *t);
/* config_overrides may be NULL. Result is fully resolved. */
void        pt_theme_resolve(const PtTheme *t, GHashTable *config_overrides,
                             PtResolvedTheme *out);
