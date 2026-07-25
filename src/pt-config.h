#pragma once
#include <glib.h>

/* Canonical terminal font size, in points, for a fresh config and for the
 * Ctrl+0 reset. pt-session.h aliases PT_FONT_SIZE_DEFAULT to this so the
 * persisted default and the config default can never drift apart. */
#define PT_CONFIG_FONT_SIZE_DEFAULT 9
/* Range the parser accepts for font-size; out-of-range values warn and keep
 * the default. Callers may clamp to a tighter range for display. */
#define PT_CONFIG_FONT_SIZE_MIN 1
#define PT_CONFIG_FONT_SIZE_MAX 256
#define PT_CONFIG_UI_FONT_SIZE_DEFAULT 12.5
#define PT_CONFIG_FONT_FAMILY_DEFAULT "JetBrains Mono"
#define PT_CONFIG_UI_FONT_FAMILY_DEFAULT "IBM Plex Sans"
#define PT_CONFIG_THEME_DEFAULT "pt-dark"

typedef struct {
  char *theme;             /* never NULL */
  int font_size;           /* terminal, points */
  char *font_family;       /* never NULL */
  double ui_font_size;     /* chrome base, px */
  char *ui_font_family;    /* never NULL */
  GHashTable *app_overrides; /* "background" (app- prefix stripped) -> "#rrggbb"/"rgba(...)" strings, both g_strdup'd */
} PtConfig;

/* Generic `key = value` walker shared with pt-theme. Skips blank lines and
 * lines starting with '#'. Malformed lines (no '=') are reported with
 * value == NULL so callers can warn. Key and value arrive trimmed. */
typedef void (*PtKvFn)(const char *key, const char *value, int lineno,
                       gpointer user);
void pt_kv_parse(const char *text, PtKvFn fn, gpointer user);

PtConfig *pt_config_new(void);                    /* all defaults */
void      pt_config_free(PtConfig *c);
PtConfig *pt_config_copy(const PtConfig *c);
gboolean  pt_config_equal(const PtConfig *a, const PtConfig *b);
PtConfig *pt_config_parse(const char *text);      /* never NULL */
char     *pt_config_default_path(void);           /* ~/.config/pt/config */
/* Rewrite managed keys in place; comments/unknown lines/ordering preserved.
 * Managed keys not present in old_text are appended at the end. app-*
 * overrides are NOT written (hand-managed). Returns new file text. */
char     *pt_config_rewrite(const char *old_text, const PtConfig *c);
/* Missing or unreadable file yields defaults. Never NULL. */
PtConfig *pt_config_load(const char *path);
/* Rewrites managed keys into the file's existing text (absent is fine) and
 * writes it back, creating the parent directory. */
gboolean  pt_config_save(const PtConfig *c, const char *path, GError **err);
