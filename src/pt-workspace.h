#pragma once

#include <glib.h>

/* The headless project/tab model behind PtWindow. Pure structure — which
 * projects exist, in what order, which tabs each one has, and what is active —
 * with none of the widgets, monitors or git state that hang off it in the UI.
 *
 * Everything is addressed by a PtWsId: stable, never reused within a session,
 * shared across projects and tabs (an id says which thing, not which kind).
 * Ids make the async seams safe by construction: a close-confirm dialog that
 * resolves after its tab is gone holds a dead id, and every call here treats a
 * dead id as a no-op — where a stored index would silently hit whatever slid
 * into the slot. */

typedef guint32 PtWsId;   /* stable, never reused within a session */

/* No project/tab. Also what the queries return for a dead or wrong-kind id. */
#define PT_WS_ID_NONE ((PtWsId)0)

/* What the index queries return for an id that names nothing (removed, never
 * existed, or a tab asked of a project query and vice versa). */
#define PT_WS_INDEX_NONE G_MAXUINT

typedef struct PtWorkspace PtWorkspace;

PtWorkspace *pt_workspace_new(void);
void pt_workspace_free(PtWorkspace *ws);

/* accent -1 = next in the fixed cycle: the project's position-to-be modulo
 * PT_ACCENT_COUNT, i.e. project_count at the moment of the add. Any other
 * value is stored as given. The first project added to an empty workspace
 * becomes active (a workspace with projects but no active one is a state the
 * UI never had). */
PtWsId pt_workspace_add_project(PtWorkspace *ws, const char *name,
                                const char *path, int accent /* -1 = next */);
/* Removing the active project selects the one that slides into its slot, or
 * the new last when it was last; removing any other project leaves the active
 * one alone — identity, not position. The project's tabs die with it (their
 * ids go dead, their data slots are dropped; freeing what the data pointed at
 * is the caller's job). A dead id is a no-op. */
void   pt_workspace_remove_project(PtWorkspace *ws, PtWsId project);
/* Reorder only: every id keeps meaning what it meant, including the active
 * ones. new_index past the end clamps to the end. */
void   pt_workspace_move_project(PtWorkspace *ws, PtWsId project,
                                 guint new_index);
/* Appends. A project's first tab becomes its active tab (same reasoning as
 * add_project). PT_WS_ID_NONE when `project` is dead. */
PtWsId pt_workspace_add_tab(PtWorkspace *ws, PtWsId project);
/* Reorder within the owning project, mirroring move_project: every id keeps
 * meaning what it meant, the active tab included. new_index past the end
 * clamps to the end. */
void   pt_workspace_move_tab(PtWorkspace *ws, PtWsId tab, guint new_index);
/* The same reorder, addressed by ids alone: `tab` lands beside `dest` — before
 * it, or after it when `after`. This is the drop shape — a drag decided over a
 * render that may have gone stale, so both ends are ids and the positions are
 * resolved here, against the order as it is now. Either id dead, the two in
 * different projects, or a resolved position that is no move at all: FALSE,
 * nothing changes. TRUE means the order moved. */
gboolean pt_workspace_move_tab_beside(PtWorkspace *ws, PtWsId tab,
                                      PtWsId dest, gboolean after);
/* Active-tab selection mirrors remove_project: the successor slides in, the
 * last falls back to the new last, a non-active removal changes nothing, an
 * empty project has no active tab. */
void   pt_workspace_remove_tab(PtWorkspace *ws, PtWsId tab);
/* Both setters ignore dead ids and ids of the wrong kind. Setting the active
 * tab of a background project does not switch projects — each project
 * remembers its own. */
void   pt_workspace_set_active_project(PtWorkspace *ws, PtWsId project);
void   pt_workspace_set_active_tab(PtWorkspace *ws, PtWsId tab);
PtWsId pt_workspace_active_project(const PtWorkspace *ws);
PtWsId pt_workspace_active_tab(const PtWorkspace *ws, PtWsId project);

guint  pt_workspace_project_count(const PtWorkspace *ws);
PtWsId pt_workspace_project_at(const PtWorkspace *ws, guint index);
guint  pt_workspace_project_index(const PtWorkspace *ws, PtWsId project);

/* Tab iteration, mirroring the project trio, plus the owner lookup the
 * grid-signal paths need (tab id → which project). */
guint  pt_workspace_tab_count(const PtWorkspace *ws, PtWsId project);
PtWsId pt_workspace_tab_at(const PtWorkspace *ws, PtWsId project, guint index);
guint  pt_workspace_tab_index(const PtWorkspace *ws, PtWsId tab);
PtWsId pt_workspace_tab_project(const PtWorkspace *ws, PtWsId tab);

/* Identity the model owns (the session file round-trips through these).
 * Borrowed strings, valid until the project is removed or the workspace
 * freed; NULL / 0 for a dead id. */
const char *pt_workspace_project_name(const PtWorkspace *ws, PtWsId project);
const char *pt_workspace_project_path(const PtWorkspace *ws, PtWsId project);
int         pt_workspace_project_accent(const PtWorkspace *ws, PtWsId project);
/* The first project whose path equals `path`, trailing slashes ignored on
 * both sides — the same directory spelled "/a/pt" and "/a/pt/" is one
 * project, not two. A project whose data slot answers TRUE through
 * `is_missing` (borrowed; NULL treats every project as present) is skipped:
 * a directory known to be gone cannot take the match. PT_WS_ID_NONE when
 * nothing matches, or `path` is NULL or empty. */
PtWsId pt_workspace_project_by_path(const PtWorkspace *ws, const char *path,
                                    gboolean (*is_missing)(gpointer data));

/* One opaque slot per id — where the UI hangs its PtProjectUI/PtTabUI. The
 * workspace never touches what it points at; the slot is dropped (not freed)
 * when its id dies. Dead ids: set is a no-op, get is NULL. */
void     pt_workspace_set_data(PtWorkspace *ws, PtWsId id, gpointer data);
gpointer pt_workspace_get_data(const PtWorkspace *ws, PtWsId id);
