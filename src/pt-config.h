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
/* What a pane's child is told $TERM is — ghostty's `term` key
 * (config/Config.zig:3724), with ghostty's default. pt earns the name: its VT
 * layer is libghostty-vt and it implements the capability set the entry
 * advertises. It is a key rather than a constant because $TERM is the one thing
 * on that list every program a user runs reads, and the entry only resolves
 * where pt's TERMINFO_DIRS reaches. sudo, su and docker exec all drop it, and
 * a user who hits that needs a way out that is not editing the source. Setting
 * this to xterm-256color is that way out.
 *
 * Only $TERM. $TERM_PROGRAM, $TERM_PROGRAM_VERSION and the XTVERSION reply are
 * not configurable and do not follow this: they describe what pt implements,
 * which does not change with the name of a terminfo entry. */
#define PT_CONFIG_TERM_DEFAULT "xterm-ghostty"

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

/* Whether a restored pane that was running a coding agent gets its session
 * resumed automatically (the agent's resume command typed into the fresh
 * shell). On: the whole point of saving the id is getting the session back. */
#define PT_CONFIG_RESUME_AGENTS_DEFAULT TRUE

/* How much history a pane keeps, in *bytes*. Not lines, whatever the C header
 * calls the field: libghostty's max_scrollback is a byte budget
 * (terminal/Screen.zig, "the amount of scrollback to keep in bytes"), rounded
 * up to a page. The default mirrors ghostty's own `scrollback-limit`
 * (config/Config.zig), 10MB. Read when a pane spawns, so — again as in
 * ghostty — a change reaches panes opened after it and leaves the ones already
 * running with the limit they were built with. */
#define PT_CONFIG_SCROLLBACK_LIMIT_DEFAULT 10000000

/* Pixels between a pane's edge and its character grid — ghostty's
 * `window-padding-x` / `window-padding-y` (config/Config.zig), whose defaults
 * are its own; these are the inset pt has always drawn. Applied live to every
 * open pane rather than only to the next one: the widget and its core take the
 * value together, so what is drawn and what pixel-to-cell mapping answers
 * (selection, links, mouse reports) can never disagree. */
#define PT_CONFIG_WINDOW_PADDING_X_DEFAULT 20
#define PT_CONFIG_WINDOW_PADDING_Y_DEFAULT 18
/* Range the parser accepts. The ceiling is well short of squeezing the grid
 * out of a pane, which is the failure ghostty's docs warn its own users to
 * avoid by hand. */
#define PT_CONFIG_WINDOW_PADDING_MIN 0
#define PT_CONFIG_WINDOW_PADDING_MAX 200

typedef struct {
  char *theme;             /* never NULL */
  int font_size;           /* terminal, points */
  char *font_family;       /* never NULL */
  double ui_font_size;     /* chrome base, px */
  char *ui_font_family;    /* never NULL */
  char *term;              /* the child's $TERM; never NULL */
  int scrollback_limit;    /* history a pane keeps, bytes */
  int window_padding_x;    /* pane edge to cell grid, px, left and right */
  int window_padding_y;    /* pane edge to cell grid, px, top and bottom */
  gboolean mouse_reporting;  /* forward mouse events to tracking apps */
  gboolean claude_usage;     /* fetch Claude Code plan usage from Anthropic */
  gboolean resume_agents;    /* resume a restored pane's agent session */
  PtOsc52Mode osc52;         /* clipboard writes from programs (OSC 52) */
   GHashTable *app_overrides; /* "background" (app- prefix stripped) -> "#rrggbb"/"rgba(...)" strings, both g_strdup'd */
   GPtrArray *binding_lines;  /* raw `bind`/`unbind` lines, in file order; see pt-bindings.h */
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

/* The raw `bind` / `unbind` lines a parse collected, in file order, for
 * pt_bindings_parse() to make sense of. Each line reads "bind <accel>
 * [action]"; the number is where it sat in the file, for warnings. */
guint      pt_config_n_binding_lines(const PtConfig *cfg);
const char *pt_config_binding_line(const PtConfig *cfg, guint i);
int        pt_config_binding_line_no(const PtConfig *cfg, guint i);
