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
/* Whether apps that ask for the mouse get the pointer: clicks, drags and
 * motion. On, as in ghostty and every other terminal. pt shipped it off for a
 * while so a plain drag selected text out of a full-screen TUI without anyone
 * having to learn a modifier first, but withholding the pointer also withholds
 * what the app does with it: Claude Code selects and copies on its own, and
 * that never started. Shift takes the pointer back for one gesture, which is
 * the override every other terminal offers. The wheel is not covered either
 * way: an app that tracks the mouse always gets it, since scrolling selects
 * nothing. See wheel_reports() in pt-terminal.c. */
#define PT_CONFIG_MOUSE_REPORTING_DEFAULT TRUE

/* What a program in a pane may do to the system clipboard with OSC 52. The
 * write half is how anything on the far end of an ssh session copies — tmux,
 * nvim's clipboard fallback, Claude Code's own selection — so it ships on. The
 * read half is not offered at any setting: see pt_term_core_set_osc52(). */
typedef enum {
  PT_OSC52_OFF = 0,   /* ignore clipboard writes entirely */
  PT_OSC52_WRITE,     /* set the clipboard, no questions asked */
  PT_OSC52_ASK,       /* confirm each write first */
} PtOsc52Mode;
#define PT_CONFIG_OSC52_DEFAULT PT_OSC52_WRITE

/* Whether the info panel may look up Claude Code's plan usage, which means
 * sending the token Claude Code stored to Anthropic. Off, and only the user
 * can turn it on — every other reader in that panel reads a local file, and
 * this is the one that puts a credential on the wire. Codex needs no such key
 * because its numbers never leave the machine. */
#define PT_CONFIG_CLAUDE_USAGE_DEFAULT FALSE

typedef struct {
  char *theme;             /* never NULL */
  int font_size;           /* terminal, points */
  char *font_family;       /* never NULL */
  double ui_font_size;     /* chrome base, px */
  char *ui_font_family;    /* never NULL */
  gboolean mouse_reporting;  /* forward mouse events to tracking apps */
  gboolean claude_usage;     /* fetch Claude Code plan usage from Anthropic */
  PtOsc52Mode osc52;         /* clipboard writes from programs (OSC 52) */
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
