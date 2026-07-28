#pragma once

#include <glib.h>
#include "pt-config.h"
#include "pt-split-tree.h"

/* Number of entries in the fixed accent cycle projects are coloured from. */
#define PT_ACCENT_COUNT 6

/* Terminal font size, in points, for a fresh session. Kept as an alias so
 * existing callers keep compiling; pt-config.h owns the value, because the
 * config file is now the source of truth for fonts and the persisted default
 * has to agree with it. */
#define PT_FONT_SIZE_DEFAULT PT_CONFIG_FONT_SIZE_DEFAULT

typedef struct { char *title; PtSplitNode *tree; } PtTabState;
typedef struct {
  char *name; char *path;
  GPtrArray *tabs;        /* PtTabState*, element free fn set */
  int active_tab;
  int accent;             /* 0..5 index into the fixed accent cycle */
} PtProjectState;
typedef struct {
  GPtrArray *projects;    /* PtProjectState*, element free fn set */
  int active_project;
  int font_size;          /* terminal font size in points */
} PtSessionState;

PtSessionState *pt_session_state_new(void);
void pt_session_state_free(PtSessionState *s);
PtTabState *pt_tab_state_new(const char *title, PtSplitNode *tree /*takes*/);
PtProjectState *pt_project_state_new(const char *name, const char *path);
char *pt_session_to_json_text(const PtSessionState *s);            /* caller frees */
PtSessionState *pt_session_from_json_text(const char *text);       /* NULL on bad */
gboolean pt_session_save(const PtSessionState *s, const char *path, GError **err);
/* Load; on unreadable/corrupt file renames it to <path>.bak and returns NULL. */
PtSessionState *pt_session_load(const char *path);
char *pt_session_default_path(void);   /* ~/.config/pt/state.json, caller frees */
/* Where `index` ends up once the project at `from` is moved to `to`. Kept
 * separate from any array because the thing that has to survive a reorder is a
 * stored position (active_project), not an element. */
int pt_session_index_after_move(int index, int from, int to);
