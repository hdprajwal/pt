#pragma once

#include <glib.h>
#include "pt-split-tree.h"

typedef struct { char *title; PtSplitNode *tree; } PtTabState;
typedef struct {
  char *name; char *path;
  GPtrArray *tabs;        /* PtTabState*, element free fn set */
  int active_tab;
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
