#include "pt-command-palette.h"
#include "pt-agent-history.h"
#include "pt-agent-session.h"
#include "pt-fuzzy.h"
#include "pt-accent.h"
#include "pt-overlay.h"
#include "pt-path.h"
#include "pt-rowlist.h"

/* The palette never scrolls: it shows the six best matches and nothing else. */
#define PT_COMMAND_PALETTE_ROWS  6
#define PT_COMMAND_PALETTE_WIDTH 620

enum { SIG_ACTIVATED, SIG_HISTORY_ACTIVATED, SIG_CLOSED, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _PtCommandPalette {
  GtkWidget parent_instance;
  PtOverlay *overlay;   /* scrim, panel, keys, dismiss, open/closed state */
  GtkWidget *entry;     /* GtkText holding the query */
  GtkWidget *list;      /* box the rows live in */
  PtRowList *rows;      /* rebuilt per query */
  GtkWidget *scope;     /* the query bar's scope label */
  PtCommandPaletteItem *items; /* owned; NULL when closed */
  int n_items;
  int *scores;          /* one per item, refilled per query; sized with items */
  int shown[PT_COMMAND_PALETTE_ROWS];  /* row -> index into items */
  int n_shown;
  int selected;         /* index into shown[]; -1 when nothing matches */
};

G_DEFINE_FINAL_TYPE(PtCommandPalette, pt_command_palette, GTK_TYPE_WIDGET)

static gboolean is_open(PtCommandPalette *p) {
  return p->overlay != NULL && pt_overlay_is_open(p->overlay);
}

/* ---------- owned items ---------- */
static void free_items(PtCommandPalette *p) {
  for (int i = 0; i < p->n_items; i++) {
    g_free(p->items[i].name);
    g_free(p->items[i].detail);
    g_free(p->items[i].shortcut);
    g_free(p->items[i].history_session_id);
    g_free(p->items[i].history_cwd);
  }
  g_clear_pointer(&p->items, g_free);
  g_clear_pointer(&p->scores, g_free);
  p->n_items = 0;
  p->n_shown = 0;
  p->selected = -1;
}

/* ---------- helpers ---------- */
/* Pick the six best matches for `q`, score descending, ties in natural order.
 * An empty query scores everything 1, so it degenerates to "the first six". */
static void filter_items(PtCommandPalette *p, const char *q) {
  /* An item is worth what its better half matches: typing a path fragment has
   * to find a project by its detail line, not only by its name. The stable
   * top-N selection over those scores is pt_fuzzy_rank_scored's job. */
  const char *needle = q != NULL ? q : "";
  for (int i = 0; i < p->n_items; i++)
    p->scores[i] = MAX(pt_fuzzy_score(needle, p->items[i].name),
                       pt_fuzzy_score(needle, p->items[i].detail));
  p->n_shown = pt_fuzzy_rank_scored(p->scores, p->n_items, p->shown,
                                    PT_COMMAND_PALETTE_ROWS);
}

/* ---------- recent agent sessions ---------- */
/* The copy glyph on a session row. Its gesture sits in the capture phase on
 * the child, so it claims the press before the row's own click controller
 * ever sees it: copying must not also resume the session. */
static void on_copy_pressed(GtkGestureClick *g, int n, double x, double y,
                            gpointer user) {
  (void)n; (void)x; (void)y;
  gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
  GtkWidget *label = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
  GdkClipboard *clip =
      gdk_display_get_clipboard(gtk_widget_get_display(label));
  gdk_clipboard_set_text(clip, user);   /* borrowed from the item */
}

static void append_copy_button(GtkWidget *row, const char *session_id) {
  GtkWidget *copy = gtk_label_new("⧉");
  gtk_widget_set_valign(copy, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(copy, "pt-palette-shortcut");
  gtk_widget_set_tooltip_text(copy, "copy session id");
  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click, "pressed", G_CALLBACK(on_copy_pressed),
                   (gpointer)session_id);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
                                             GTK_PHASE_CAPTURE);
  gtk_widget_add_controller(copy, GTK_EVENT_CONTROLLER(click));
  gtk_box_append(GTK_BOX(row), copy);
}

/* Swap the row model to recent agent sessions: one item per readable report,
 * newest first. Built here rather than by the window so the trigger row stays
 * a plain command and the window never learns how reports are read. Escape
 * closes — back out is ^K again, not a third list to track. */
static void enter_history_mode(PtCommandPalette *p) {
  char *dir = pt_agent_session_dir();
  GPtrArray *hist = pt_agent_history_load(dir);
  g_free(dir);
  GDateTime *now = g_date_time_new_now_local();
  GArray *arr = g_array_new(FALSE, TRUE, sizeof(PtCommandPaletteItem));

  if (hist->len == 0) {
    /* The friendly empty state, as a dead row so Enter on it does nothing. */
    PtCommandPaletteItem none = {
      .name = g_strdup("No recent agent sessions"),
      .detail = g_strdup("one lands here when an agent starts in a pane"),
      .shortcut = NULL, .accent = 0, .is_shell = FALSE, .is_command = FALSE,
      .is_history = TRUE, .history_dead = TRUE,
      .project_id = 0, .tab_id = 0, .command = -1,
    };
    g_array_append_val(arr, none);
  }
  for (guint i = 0; i < hist->len; i++) {
    PtAgentHistoryEntry *e = g_ptr_array_index(hist, i);
    gboolean dead = e->cwd == NULL ||
                    !g_file_test(e->cwd, G_FILE_TEST_IS_DIR);
    char shown[512] = "";   /* dead rows show "?" below, never a stale buffer */
    if (e->cwd != NULL)
      pt_path_home_abbrev(e->cwd, g_get_home_dir(), shown, sizeof shown);
    char *rel = pt_agent_history_relative_time(e->ts, now);
    const char *agent = pt_agent_session_kind_name(e->agent);
    PtCommandPaletteItem it = {
      .name = dead ? g_strdup("[missing]") : g_path_get_basename(e->cwd),
      .detail = g_strdup_printf("%s · %s · %s", agent != NULL ? agent : "?",
                                dead ? "?" : shown, rel),
      .shortcut = NULL, .accent = 0, .is_shell = FALSE, .is_command = FALSE,
      .is_history = TRUE, .history_dead = dead, .history_agent = e->agent,
      .history_session_id = g_strdup(e->session_id),
      .history_cwd = g_strdup(e->cwd),
      /* Switch-target ids mean nothing here; activation goes through
       * "history-activated" with copied strings instead. */
      .project_id = 0, .tab_id = 0, .command = -1,
    };
    g_free(rel);
    g_array_append_val(arr, it);
  }
  g_date_time_unref(now);
  g_ptr_array_unref(hist);

  /* Swap in the new block through open()'s own path: it drops the old items,
   * resizes the score room and rebuilds. The query is cleared — a filter that
   * matched projects says nothing about sessions. */
  int n = (int)arr->len;
  pt_command_palette_open(p, (PtCommandPaletteItem *)g_array_free(arr, FALSE),
                          n);
  gtk_label_set_text(GTK_LABEL(p->scope), "recent agent sessions");
}

/* ---------- row rendering ---------- */
static GtkWidget *build_row(gpointer items, guint idx, gpointer user) {
  PtCommandPalette *p = user;
  const PtCommandPaletteItem *it =
      &((const PtCommandPaletteItem *)items)[p->shown[idx]];

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(row, "pt-palette-row");
  if ((int)idx == p->selected) gtk_widget_add_css_class(row, "selected");

  GtkWidget *dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(dot, "pt-dot");
  gtk_widget_add_css_class(dot, "pt-dot-6");
  pt_accent_set_class(dot, it->accent);
  gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(row), dot);

  GtkWidget *name = gtk_label_new(it->name);
  gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(name, TRUE);
  gtk_widget_add_css_class(name, "pt-palette-name");
  gtk_box_append(GTK_BOX(row), name);

  GtkWidget *detail = gtk_label_new(it->detail != NULL ? it->detail : "");
  gtk_label_set_xalign(GTK_LABEL(detail), 1.0f);
  gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_START);
  gtk_widget_add_css_class(detail, "pt-palette-detail");
  gtk_box_append(GTK_BOX(row), detail);

  GtkWidget *kind = gtk_label_new(it->is_command ? "COMMAND"
                                  : it->is_shell ? "SHELL"
                                  : it->is_history ? "AGENT" : "PROJECT");
  gtk_widget_set_valign(kind, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(kind, "pt-palette-kind");
  gtk_box_append(GTK_BOX(row), kind);

  if (it->is_history && !it->history_dead)
    append_copy_button(row, it->history_session_id);

  GtkWidget *sc = gtk_label_new(it->shortcut != NULL ? it->shortcut : "");
  gtk_widget_set_valign(sc, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(sc, "pt-palette-shortcut");
  gtk_box_append(GTK_BOX(row), sc);
  return row;
}

/* The rows are cheap and every keystroke re-ranks them, so there is nothing to
 * dedupe against: no items_equal, and the items block stays the palette's (the
 * row list borrows it, and clear_rows below runs before it is freed). */
static void rebuild(PtCommandPalette *p) {
  if (!is_open(p)) return;
  filter_items(p, gtk_editable_get_text(GTK_EDITABLE(p->entry)));
  /* The list can shrink under a selection that was valid a keystroke ago. */
  if (p->selected >= p->n_shown) p->selected = p->n_shown - 1;
  if (p->selected < 0 && p->n_shown > 0) p->selected = 0;
  if (p->n_shown == 0) p->selected = -1;

  pt_rowlist_set(p->rows, p->items, (guint)p->n_shown, build_row, NULL, p, NULL);
}

static void clear_rows(PtCommandPalette *p) {
  pt_rowlist_set(p->rows, NULL, 0, build_row, NULL, p, NULL);
}

/* ---------- activation ---------- */
static void activate_selected(PtCommandPalette *p) {
  if (!is_open(p)) return;
  if (p->selected < 0 || p->selected >= p->n_shown) {
    pt_command_palette_close(p);
    return;
  }
  const PtCommandPaletteItem *it = &p->items[p->shown[p->selected]];
  /* The mode switch never reaches the window, and it swaps the items block
   * `it` lives in — handled first, with nothing read from `it` afterwards. */
  if (it->is_command && it->command == PT_COMMAND_PALETTE_RECENT_SESSIONS) {
    enter_history_mode(p);
    return;
  }
  /* A dead session row (cwd gone on disk, or the empty-list note) answers
   * like a dead id: the palette closes and nothing else happens. */
  if (it->is_history) {
    if (it->history_dead) {
      pt_command_palette_close(p);
      return;
    }
    /* Copy before emitting: close() below frees the strings. */
    int agent = (int)it->history_agent;
    char *session_id = g_strdup(it->history_session_id);
    char *cwd = g_strdup(it->history_cwd);
    g_signal_emit(p, signals[SIG_HISTORY_ACTIVATED], 0,
                  agent, session_id, cwd);
    g_free(session_id);
    g_free(cwd);
    pt_command_palette_close(p);
    return;
  }
  guint project_id = it->project_id;
  guint tab_id = it->tab_id;
  int command = it->is_command ? it->command : -1;
  g_signal_emit(p, signals[SIG_ACTIVATED], 0, project_id, tab_id, command);
  pt_command_palette_close(p);
}

static void on_row_activated(PtRowList *rl, int idx, gpointer user) {
  (void)rl;
  PtCommandPalette *p = user;
  p->selected = idx;
  activate_selected(p);
}

/* ---------- input ---------- */
static void on_query_changed(GtkEditable *ed, gpointer user) {
  (void)ed;
  PtCommandPalette *p = user;
  /* Every edit re-ranks the list, so the old highlight means nothing. */
  p->selected = 0;
  rebuild(p);
}

static void move_selection(PtCommandPalette *p, int delta) {
  if (p->n_shown == 0) return;
  int next = p->selected + delta;
  p->selected = CLAMP(next, 0, p->n_shown - 1);
  /* Highlight without rebuilding: arrow keys must not destroy the rows a click
   * gesture might be sitting on. */
  pt_rowlist_mark_selected(p->list, p->selected);
}

/* Runs in the overlay's CAPTURE phase, and only while the palette is open. */
static gboolean on_key(guint keyval, GdkModifierType state, gpointer user) {
  PtCommandPalette *p = user;
  switch (keyval) {
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
      move_selection(p, 1);
      return TRUE;
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
      move_selection(p, -1);
      return TRUE;
    /* Tab steps the selection rather than moving focus; the overlay traps it
     * either way, so it can never reach the terminal underneath. */
    case GDK_KEY_Tab:
    case GDK_KEY_KP_Tab:
      move_selection(p, (state & GDK_SHIFT_MASK) != 0 ? -1 : 1);
      return TRUE;
    case GDK_KEY_ISO_Left_Tab:
      move_selection(p, -1);
      return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter:
      activate_selected(p);
      return TRUE;
    case GDK_KEY_Escape: {
      const char *q = gtk_editable_get_text(GTK_EDITABLE(p->entry));
      /* First Escape empties a non-empty query; the next one closes. */
      if (q != NULL && q[0] != '\0')
        gtk_editable_set_text(GTK_EDITABLE(p->entry), "");  /* fires changed */
      else
        pt_command_palette_close(p);
      return TRUE;
    }
    default:
      return FALSE;
  }
}

/* A press outside the panel closes, query or no query. */
static void on_dismissed(PtOverlay *o, gpointer user) {
  (void)o;
  pt_command_palette_close(PT_COMMAND_PALETTE(user));
}

/* The overlay is already hidden by here; what is left is what it was showing. */
static void on_overlay_closed(PtOverlay *o, gpointer user) {
  (void)o;
  PtCommandPalette *p = PT_COMMAND_PALETTE(user);
  clear_rows(p);
  free_items(p);
  g_signal_emit(p, signals[SIG_CLOSED], 0);
}

/* ---------- public API ---------- */
void pt_command_palette_open(PtCommandPalette *p, PtCommandPaletteItem *items,
                             int n_items) {
  g_return_if_fail(PT_IS_COMMAND_PALETTE(p));
  clear_rows(p);   /* the rows borrow the items free_items is about to drop */
  free_items(p);
  p->items = items;
  /* A projectless window hands over a NULL array; the palette opens empty. */
  p->n_items = (items != NULL && n_items > 0) ? n_items : 0;
  /* Scored once per keystroke, so the room for the scores is taken here and
   * not on the typing path. */
  p->scores = p->n_items > 0 ? g_new0(int, (gsize)p->n_items) : NULL;
  p->selected = 0;
  /* Every open starts from the main list; enter_history_mode re-opens with
   * its own label after this. */
  gtk_label_set_text(GTK_LABEL(p->scope), "projects · shells · commands");
  pt_overlay_open(p->overlay);

  /* Setting the text fires "changed" only when it actually changes, so rebuild
   * unconditionally afterwards. */
  gtk_editable_set_text(GTK_EDITABLE(p->entry), "");
  rebuild(p);
  gtk_widget_grab_focus(p->entry);
}

void pt_command_palette_close(PtCommandPalette *p) {
  g_return_if_fail(PT_IS_COMMAND_PALETTE(p));
  if (p->overlay != NULL) pt_overlay_close(p->overlay);
}

gboolean pt_command_palette_is_open(PtCommandPalette *p) {
  g_return_val_if_fail(PT_IS_COMMAND_PALETTE(p), FALSE);
  return is_open(p);
}

/* ---------- GObject ---------- */
/* No "closed" here on purpose: the window is on its way out too, and its
 * handler would reach for panes that dispose has already dropped. Dropping the
 * overlay is what takes the scrim down, and it emits nothing either. */
static void pt_command_palette_dispose(GObject *obj) {
  PtCommandPalette *p = PT_COMMAND_PALETTE(obj);
  /* Overlay first: it takes the rows down, and a row must never outlive the row
   * list its gesture points at. */
  g_clear_object(&p->overlay);
  g_clear_object(&p->rows);
  free_items(p);
  p->entry = p->list = NULL;
  G_OBJECT_CLASS(pt_command_palette_parent_class)->dispose(obj);
}

static void pt_command_palette_class_init(PtCommandPaletteClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_command_palette_dispose;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass),
                                           GTK_TYPE_BIN_LAYOUT);
  signals[SIG_ACTIVATED] = g_signal_new("activated", PT_TYPE_COMMAND_PALETTE,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
      G_TYPE_UINT, G_TYPE_UINT, G_TYPE_INT);
  signals[SIG_HISTORY_ACTIVATED] =
      g_signal_new("history-activated", PT_TYPE_COMMAND_PALETTE,
          G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
          G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
  signals[SIG_CLOSED] = g_signal_new("closed", PT_TYPE_COMMAND_PALETTE,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void pt_command_palette_init(PtCommandPalette *p) {
  p->selected = -1;

  /* ".pt-palette", not ".pt-command-palette": the CSS names stay put on
   * purpose — see the note in the header. */
  p->overlay = pt_overlay_new(GTK_WIDGET(p), "pt-palette");
  GtkBox *panel = pt_overlay_panel(p->overlay);
  gtk_widget_set_size_request(GTK_WIDGET(panel), PT_COMMAND_PALETTE_WIDTH, -1);
  pt_overlay_set_key_handler(p->overlay, on_key, p);
  g_signal_connect(p->overlay, "dismissed", G_CALLBACK(on_dismissed), p);
  g_signal_connect(p->overlay, "closed", G_CALLBACK(on_overlay_closed), p);

  GtkWidget *query = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(query, "pt-palette-query");
  GtkWidget *glyph = gtk_label_new("⌕");
  gtk_widget_add_css_class(glyph, "pt-search-glyph");
  gtk_box_append(GTK_BOX(query), glyph);
  p->entry = gtk_text_new();
  gtk_widget_set_hexpand(p->entry, TRUE);
  g_signal_connect(p->entry, "changed", G_CALLBACK(on_query_changed), p);
  gtk_box_append(GTK_BOX(query), p->entry);
  GtkWidget *scope = gtk_label_new("projects · shells · commands");
  gtk_widget_set_valign(scope, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(scope, "pt-palette-scope");
  gtk_box_append(GTK_BOX(query), scope);
  p->scope = scope;
  gtk_box_append(panel, query);

  p->list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(panel, p->list);
  p->rows = pt_rowlist_new(GTK_BOX(p->list));
  /* Connected before the first rebuild: that is what puts a click gesture on
   * the rows. */
  g_signal_connect(p->rows, "row-activated", G_CALLBACK(on_row_activated), p);
}

GtkWidget *pt_command_palette_new(void) {
  return g_object_new(PT_TYPE_COMMAND_PALETTE, NULL);
}
