#pragma once
#include <glib.h>
#include "pt-split-tree.h"

/* What a configured shortcut does. The enum lives beside the binding parser
 * rather than in pt-window.c so the one name -> (id, arg) table below and the
 * window's dispatch switch can never disagree about the list; the window maps
 * each id onto its handler. */
typedef enum {
  PT_ACTION_SWITCH_PROJECT,
  PT_ACTION_SWITCH_TAB,
  PT_ACTION_NEW_TAB,
  PT_ACTION_ADD_PROJECT,
  PT_ACTION_TOGGLE_SIDEBAR,
  PT_ACTION_TOGGLE_INFOPANEL,
  PT_ACTION_NEXT_TAB,
  PT_ACTION_PREV_TAB,
  PT_ACTION_NEXT_PROJECT,
  PT_ACTION_PREV_PROJECT,
  PT_ACTION_SPLIT,
  PT_ACTION_CLOSE_PANE,
  PT_ACTION_FOCUS_NEXT,
  PT_ACTION_FOCUS_PREV,
  PT_ACTION_FOCUS_DIRECTION,
  PT_ACTION_PASTE,
  PT_ACTION_COPY,
  PT_ACTION_ZOOM,
  PT_ACTION_ZOOM_PANE,
} PtActionId;

/* One parsed `bind` / `unbind` config line. accel is the canonical GTK
 * spelling ("<Control><Shift>t"), normalized from the config's lowercase
 * grammar so the window layer can hand it straight to the shortcut trigger
 * parser. action is NULL on an unbind. */
typedef struct {
  char *accel;
  char *action;
  gboolean is_unbind;
  int line_no;
} PtBindingLine;

/* One row of the action-name space: the config spelling of one (id, arg)
 * pair. This is the whole vocabulary a config may name — anything else is a
 * warning, not silently ignored typing. */
typedef struct {
  const char *name;
  PtActionId id;
  int arg;
} PtBindingAction;

/* Resolve a config action name (case-insensitive) to its id and argument.
 * FALSE means the name is not in the space. */
gboolean pt_bindings_action_lookup(const char *name, PtActionId *id, int *arg);

/* Parse raw `bind` / `unbind` config lines into the effective binding set:
 * one entry per accelerator, a later line replacing an earlier one for the
 * same accelerator. Lines that do not parse (bad accelerator, unknown
 * action, missing pieces) warn naming the offending line and are skipped;
 * the rest still apply. lines[i] is a full stripped config line ("bind
 * ctrl+shift+t new-tab"); line_nos[i] its 1-based source line, or NULL to
 * number them 1..n. Never NULL; error is reserved and never set today. */
GPtrArray *pt_bindings_parse(const char *const *lines, const int *line_nos,
                             GError **error);
void pt_bindings_free(GPtrArray *bindings);
