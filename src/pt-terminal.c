#include "pt-terminal.h"
#include "pt-keymap.h"
#include "pt-session.h"      /* PT_FONT_SIZE_DEFAULT, shared with persistence */
#include <math.h>

#define PT_FONT_FAMILY "JetBrains Mono, monospace"
#define PT_FONT_SIZE_MIN 6
#define PT_FONT_SIZE_MAX 32
/* Inset between the pane edge and the character grid (mirrored by
 * PT_CORE_PAD_X / PT_CORE_PAD_Y in pt-term-core.c for hit-testing). */
#define PT_PAD_X 20
#define PT_PAD_Y 18

/* One font size shared by every terminal: zoom is global, not per-pane. Live
 * widgets register here so a size change can re-measure them all. */
static int font_size_pts = PT_FONT_SIZE_DEFAULT;
static GSList *live_terminals;

/* Env applied by pt_terminal_new when the caller has no project context
 * (pane grids build terminals straight from split-tree leaves). */
static char **default_env;

enum { SIG_EXITED, SIG_TITLE_CHANGED, SIG_ACTIVITY, SIG_COMMAND_CHANGED,
       N_SIGNALS };
static guint signals[N_SIGNALS];

struct _PtTerminal {
  GtkWidget parent_instance;
  PtTermCore *core;
  char *start_cwd;
  char **env;                /* extra child env, or NULL */
  char *last_command;
  PangoLayout *layout;
  PangoFontDescription *font_desc;
  int cell_w, cell_h;
  gboolean exited;
  int exit_status;
  gboolean focused;
};

G_DEFINE_FINAL_TYPE(PtTerminal, pt_terminal, GTK_TYPE_WIDGET)

/* ---- core callbacks ---- */
static void core_draw(PtTermCore *core, gpointer user) {
  (void)core;
  PtTerminal *t = PT_TERMINAL(user);
  gtk_widget_queue_draw(GTK_WIDGET(t));
  g_signal_emit(t, signals[SIG_ACTIVITY], 0);
}

static void core_exited(PtTermCore *core, int status, gpointer user) {
  (void)core;
  PtTerminal *t = PT_TERMINAL(user);
  t->exited = TRUE;
  t->exit_status = status;
  gtk_widget_queue_draw(GTK_WIDGET(t));
  g_signal_emit(t, signals[SIG_EXITED], 0, status);
}

static void core_title(PtTermCore *core, const char *title, gpointer user) {
  (void)core;
  g_signal_emit(PT_TERMINAL(user), signals[SIG_TITLE_CHANGED], 0, title);
}

static void core_command(PtTermCore *core, const char *comm, gpointer user) {
  (void)core;
  PtTerminal *t = PT_TERMINAL(user);
  g_free(t->last_command);
  t->last_command = g_strdup(comm);
  g_signal_emit(t, signals[SIG_COMMAND_CHANGED], 0, comm);
}

/* ---- geometry ---- */
static void measure_font(PtTerminal *t) {
  PangoContext *pc = gtk_widget_get_pango_context(GTK_WIDGET(t));
  PangoFontMetrics *m =
      pango_context_get_metrics(pc, t->font_desc, NULL);
  t->cell_w = PANGO_PIXELS(pango_font_metrics_get_approximate_digit_width(m));
  t->cell_h = PANGO_PIXELS(pango_font_metrics_get_ascent(m) +
                           pango_font_metrics_get_descent(m));
  pango_font_metrics_unref(m);
  if (t->cell_w < 1) t->cell_w = 8;
  if (t->cell_h < 1) t->cell_h = 16;
}

/* Design palette: the ANSI slots pt overrides so status output matches the
 * app chrome. Everything else keeps libghostty's built-in defaults. */
static void apply_palette(PtTermCore *core) {
  GhosttyTerminal term = pt_term_core_terminal(core);
  GhosttyColorRgb palette[256];
  if (ghostty_terminal_get(term, GHOSTTY_TERMINAL_DATA_COLOR_PALETTE_DEFAULT,
                           palette) != GHOSTTY_SUCCESS)
    return;
  const GhosttyColorRgb red = { 0xf2, 0x77, 0x7a };
  const GhosttyColorRgb green = { 0x6e, 0xe7, 0xa0 };
  const GhosttyColorRgb yellow = { 0xf2, 0xb2, 0x5c };
  palette[1] = palette[9] = red;
  palette[2] = palette[10] = green;
  palette[3] = palette[11] = yellow;
  ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, palette);
}

static void ensure_core(PtTerminal *t) {
  if (t->core != NULL) return;
  measure_font(t);
  GError *err = NULL;
  t->core = pt_term_core_new(t->start_cwd, NULL,
                             (const char *const *)t->env, 80, 24,
                             t->cell_w, t->cell_h, &err);
  if (t->core == NULL) {
    g_warning("pt: terminal spawn failed: %s",
              err != NULL ? err->message : "?");
    g_clear_error(&err);
    t->exited = TRUE;
    t->exit_status = -1;
    return;
  }
  apply_palette(t->core);
  PtTermCoreCallbacks cbs = { .draw = core_draw, .exited = core_exited,
                              .title = core_title, .command = core_command };
  pt_term_core_set_callbacks(t->core, &cbs, t);
}

static void pt_terminal_size_allocate(GtkWidget *widget, int width, int height,
                                      int baseline) {
  (void)baseline;
  PtTerminal *t = PT_TERMINAL(widget);
  ensure_core(t);
  if (t->core == NULL) return;
  int cols = (width - 2 * PT_PAD_X) / t->cell_w;
  int rows = (height - 2 * PT_PAD_Y) / t->cell_h;
  if (cols < 2) cols = 2;
  if (rows < 2) rows = 2;
  pt_term_core_resize(t->core, (guint16)cols, (guint16)rows,
                      t->cell_w, t->cell_h);
}

/* ---- rendering ---- */
static void pt_terminal_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  PtTerminal *t = PT_TERMINAL(widget);
  int w = gtk_widget_get_width(widget);
  int h = gtk_widget_get_height(widget);

  ensure_core(t);
  if (t->core == NULL) {
    gtk_snapshot_append_color(snapshot,
        &(GdkRGBA){0x0b / 255.0f, 0x0d / 255.0f, 0x10 / 255.0f, 1},
        &GRAPHENE_RECT_INIT(0, 0, w, h));
    return;
  }
  pt_term_core_sync(t->core);
  GhosttyRenderState rs = pt_term_core_render_state(t->core);

  /* Default/effective colors: libghostty-vt exposes these as individual
   * render-state queries (no aggregate colors struct in this ABI). */
  GhosttyColorRgb bg_default = { 0x0b, 0x0d, 0x10 };
  GhosttyColorRgb fg_default = { 0xd6, 0xda, 0xe0 };
  ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_COLOR_BACKGROUND,
                           &bg_default);
  ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_COLOR_FOREGROUND,
                           &fg_default);
  bool cursor_has_value = false;
  ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_COLOR_CURSOR_HAS_VALUE,
                           &cursor_has_value);
  GhosttyColorRgb cursor_color = fg_default;
  if (cursor_has_value)
    ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_COLOR_CURSOR,
                             &cursor_color);

  gtk_snapshot_append_color(snapshot,
      &(GdkRGBA){ bg_default.r / 255.0f, bg_default.g / 255.0f,
                  bg_default.b / 255.0f, 1.0f },
      &GRAPHENE_RECT_INIT(0, 0, w, h));

  GhosttyRenderStateRowIterator iter = pt_term_core_row_iter(t->core);
  if (ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                               &iter) != GHOSTTY_SUCCESS)
    return;

  char text[64];
  int y = PT_PAD_Y;
  while (ghostty_render_state_row_iterator_next(iter)) {
    GhosttyRenderStateRowCells cells = pt_term_core_row_cells(t->core);
    if (ghostty_render_state_row_get(iter, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                     &cells) != GHOSTTY_SUCCESS)
      continue;
    int x = PT_PAD_X;
    while (ghostty_render_state_row_cells_next(cells)) {
      uint32_t glen = 0;
      ghostty_render_state_row_cells_get(cells,
          GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &glen);

      bool selected = false;
      ghostty_render_state_row_cells_get(cells,
          GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_SELECTED, &selected);
      /* selection highlight (#264f38) takes over the cell background */
      GdkRGBA sel_rgba = { 0x26 / 255.0f, 0x4f / 255.0f, 0x38 / 255.0f, 1.0f };

      GhosttyColorRgb fg = fg_default;
      GhosttyColorRgb bg = bg_default;
      gboolean has_bg = ghostty_render_state_row_cells_get(cells,
          GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg) == GHOSTTY_SUCCESS;

      if (glen == 0) {
        if (selected)
          gtk_snapshot_append_color(snapshot, &sel_rgba,
              &GRAPHENE_RECT_INIT(x, y, t->cell_w, t->cell_h));
        else if (has_bg)
          gtk_snapshot_append_color(snapshot,
              &(GdkRGBA){bg.r / 255.0f, bg.g / 255.0f, bg.b / 255.0f, 1},
              &GRAPHENE_RECT_INIT(x, y, t->cell_w, t->cell_h));
        x += t->cell_w;
        continue;
      }

      if (ghostty_render_state_row_cells_get(cells,
          GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg) != GHOSTTY_SUCCESS)
        fg = fg_default;
      GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
      ghostty_render_state_row_cells_get(cells,
          GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
      if (style.inverse) {
        GhosttyColorRgb tmp = fg; fg = bg; bg = tmp; has_bg = TRUE;
      }
      if (selected)
        gtk_snapshot_append_color(snapshot, &sel_rgba,
            &GRAPHENE_RECT_INIT(x, y, t->cell_w, t->cell_h));
      else if (has_bg)
        gtk_snapshot_append_color(snapshot,
            &(GdkRGBA){bg.r / 255.0f, bg.g / 255.0f, bg.b / 255.0f, 1},
            &GRAPHENE_RECT_INIT(x, y, t->cell_w, t->cell_h));

      /* GRAPHEMES_BUF writes glen codepoints — buffer must hold ALL of them
       * (see Task 5 review: stack overflow otherwise). */
      uint32_t cps_stack[16];
      uint32_t *cps = glen <= 16 ? cps_stack : g_new(uint32_t, glen);
      ghostty_render_state_row_cells_get(cells,
          GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, cps);
      int pos = 0;
      for (uint32_t i = 0; i < glen && pos < 60; i++)
        pos += g_unichar_to_utf8((gunichar)cps[i], text + pos);
      text[pos] = '\0';
      if (cps != cps_stack) g_free(cps);

      pango_layout_set_text(t->layout, text, pos);
      pango_layout_set_attributes(t->layout, NULL);
      if (style.bold || style.italic) {
        PangoAttrList *attrs = pango_attr_list_new();
        if (style.bold)
          pango_attr_list_insert(attrs,
              pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        if (style.italic)
          pango_attr_list_insert(attrs,
              pango_attr_style_new(PANGO_STYLE_ITALIC));
        pango_layout_set_attributes(t->layout, attrs);
        pango_attr_list_unref(attrs);
      }
      gtk_snapshot_save(snapshot);
      gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(x, y));
      gtk_snapshot_append_layout(snapshot, t->layout,
          &(GdkRGBA){fg.r / 255.0f, fg.g / 255.0f, fg.b / 255.0f, 1});
      gtk_snapshot_restore(snapshot);
      x += t->cell_w;
    }
    y += t->cell_h;
  }

  /* cursor */
  bool cur_visible = false, cur_in_vp = false;
  ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE,
                           &cur_visible);
  ghostty_render_state_get(rs,
      GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE, &cur_in_vp);
  if (cur_visible && cur_in_vp && !t->exited) {
    uint16_t cx = 0, cy = 0;
    ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cx);
    ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cy);
    GhosttyColorRgb cc = cursor_has_value ? cursor_color : fg_default;
    gtk_snapshot_append_color(snapshot,
        &(GdkRGBA){cc.r / 255.0f, cc.g / 255.0f, cc.b / 255.0f, 0.55f},
        &GRAPHENE_RECT_INIT(PT_PAD_X + cx * t->cell_w,
                            PT_PAD_Y + cy * t->cell_h,
                            t->cell_w, t->cell_h));
  }

  /* focused-pane ring: an inset 1px dark-green border drawn on top of the
   * content (the CSS box-shadow would be painted over by our bg fill). */
  if (t->focused) {
    GdkRGBA ring = { 0x2f / 255.0f, 0x4f / 255.0f, 0x3a / 255.0f, 1.0f };
    GskRoundedRect rr;
    gsk_rounded_rect_init_from_rect(&rr, &GRAPHENE_RECT_INIT(0, 0, w, h), 0);
    gtk_snapshot_append_border(snapshot, &rr,
        (float[4]){ 1, 1, 1, 1 },
        (GdkRGBA[4]){ ring, ring, ring, ring });
  }

  /* exited banner */
  if (t->exited) {
    char msg[96];
    g_snprintf(msg, sizeof(msg),
               "[process exited: %d]  Enter=restart  Ctrl+Shift+W=close",
               t->exit_status);
    gtk_snapshot_append_color(snapshot, &(GdkRGBA){0, 0, 0, 0.75f},
        &GRAPHENE_RECT_INIT(0, h - t->cell_h - 8, w, t->cell_h + 8));
    pango_layout_set_attributes(t->layout, NULL);
    pango_layout_set_text(t->layout, msg, -1);
    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot,
                           &GRAPHENE_POINT_INIT(PT_PAD_X, h - t->cell_h - 4));
    gtk_snapshot_append_layout(snapshot, t->layout,
                               &(GdkRGBA){0.9f, 0.76f, 0.48f, 1});
    gtk_snapshot_restore(snapshot);
  }
}

/* ---- input ---- */
static void restart_shell(PtTerminal *t) {
  char *cwd = pt_terminal_current_cwd(t);
  if (t->core != NULL) pt_term_core_free(t->core);
  t->core = NULL;
  t->exited = FALSE;
  g_free(t->start_cwd);
  t->start_cwd = cwd != NULL ? cwd : g_strdup(g_get_home_dir());
  ensure_core(t);
  gtk_widget_queue_allocate(GTK_WIDGET(t)); /* re-sizes the new core */
  gtk_widget_queue_draw(GTK_WIDGET(t));
}

static void on_focus_enter(GtkEventControllerFocus *ctl, gpointer user) {
  (void)ctl;
  PtTerminal *t = PT_TERMINAL(user);
  t->focused = TRUE;
  gtk_widget_add_css_class(GTK_WIDGET(t), "focused");
  gtk_widget_queue_draw(GTK_WIDGET(t));
}

static void on_focus_leave(GtkEventControllerFocus *ctl, gpointer user) {
  (void)ctl;
  PtTerminal *t = PT_TERMINAL(user);
  t->focused = FALSE;
  gtk_widget_remove_css_class(GTK_WIDGET(t), "focused");
  gtk_widget_queue_draw(GTK_WIDGET(t));
}

static gboolean on_key_pressed(GtkEventControllerKey *ctl, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user) {
  (void)ctl; (void)keycode;
  PtTerminal *t = PT_TERMINAL(user);
  if (t->exited) {
    if (keyval == GDK_KEY_Return) { restart_shell(t); return TRUE; }
    return FALSE;
  }
  if (t->core == NULL) return FALSE;

  GhosttyKey key = pt_keymap_from_keyval(keyval);
  GhosttyMods mods = pt_keymap_mods(state);
  guint32 unshifted = pt_keymap_unshifted_codepoint(keyval);

  char utf8[8];
  gsize utf8_len = 0;
  guint32 uc = gdk_keyval_to_unicode(keyval);
  if (uc != 0 && g_unichar_isprint(uc) &&
      (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) == 0)
    utf8_len = g_unichar_to_utf8((gunichar)uc, utf8);

  gboolean consumed =
      pt_term_core_send_key(t->core, key, GHOSTTY_KEY_ACTION_PRESS, mods,
                            unshifted, utf8, utf8_len);
  if (consumed) {
    /* any keypress that writes to the pty drops the selection */
    pt_term_core_selection_clear(t->core);
    gtk_widget_queue_draw(GTK_WIDGET(t));
  }
  return consumed;
}

static gboolean on_scroll(GtkEventControllerScroll *ctl, double dx, double dy,
                          gpointer user) {
  (void)ctl; (void)dx;
  PtTerminal *t = PT_TERMINAL(user);
  if (t->core == NULL) return FALSE;
  /* When an app tracks the mouse (vim, htop), it owns the wheel; v1 forwards
   * nothing in that case rather than corrupting viewport state. */
  if (pt_term_core_mouse_tracking(t->core)) return TRUE;
  pt_term_core_scroll_delta(t->core, dy > 0 ? 3 : -3);
  return TRUE;
}

static void on_click_pressed(GtkGestureClick *g, int n, double x, double y,
                             gpointer user) {
  (void)n;
  PtTerminal *t = PT_TERMINAL(user);
  gtk_widget_grab_focus(GTK_WIDGET(t));
  if (t->core == NULL) return;
  /* controller event time is in milliseconds; the core wants nanoseconds */
  guint32 ms =
      gtk_event_controller_get_current_event_time(GTK_EVENT_CONTROLLER(g));
  pt_term_core_selection_press(t->core, x, y, (guint64)ms * 1000000ULL);
  gtk_widget_queue_draw(GTK_WIDGET(t));
}

static void on_click_released(GtkGestureClick *g, int n, double x, double y,
                              gpointer user) {
  (void)g; (void)n;
  PtTerminal *t = PT_TERMINAL(user);
  if (t->core == NULL) return;
  pt_term_core_selection_release(t->core, x, y);
  gtk_widget_queue_draw(GTK_WIDGET(t));
}

static void on_drag_update(GtkGestureDrag *g, double ox, double oy,
                           gpointer user) {
  PtTerminal *t = PT_TERMINAL(user);
  if (t->core == NULL) return;
  double sx = 0, sy = 0;
  gtk_gesture_drag_get_start_point(g, &sx, &sy);
  pt_term_core_selection_drag(t->core, sx + ox, sy + oy);
  gtk_widget_queue_draw(GTK_WIDGET(t));
}

static void on_paste_text(GObject *src, GAsyncResult *res, gpointer user) {
  PtTerminal *t = PT_TERMINAL(user);
  char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
  if (text == NULL) { g_object_unref(t); return; }
  if (t->core != NULL) {
    if (pt_term_core_bracketed_paste(t->core)) {
      pt_term_core_write(t->core, "\x1b[200~", 6);
      pt_term_core_write(t->core, text, -1);
      pt_term_core_write(t->core, "\x1b[201~", 6);
    } else {
      pt_term_core_write(t->core, text, -1);
    }
  }
  g_free(text);
  g_object_unref(t);
}

void pt_terminal_paste(PtTerminal *t) {
  GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(t));
  gdk_clipboard_read_text_async(cb, NULL, on_paste_text, g_object_ref(t));
}

void pt_terminal_copy(PtTerminal *t) {
  if (t->core == NULL) return;
  char *text = pt_term_core_selection_text(t->core);   /* g_strndup'd */
  if (text == NULL) return;
  gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(t)), text);
  g_free(text);
}

char *pt_terminal_current_cwd(PtTerminal *t) {
  if (t->core == NULL) return g_strdup(t->start_cwd);
  char proc[64];
  g_snprintf(proc, sizeof(proc), "/proc/%d/cwd",
             (int)pt_term_core_shell_pid(t->core));
  char *cwd = g_file_read_link(proc, NULL);
  return cwd != NULL ? cwd : g_strdup(t->start_cwd);
}

PtTermCore *pt_terminal_core(PtTerminal *t) { return t->core; }

const char *pt_terminal_last_command(PtTerminal *t) { return t->last_command; }

gboolean pt_terminal_running(PtTerminal *t) {
  return t->core != NULL && pt_term_core_running(t->core);
}

int pt_terminal_last_exit(PtTerminal *t) {
  return t->core != NULL ? pt_term_core_last_exit(t->core) : -1;
}

/* ---- global font zoom ---- */
int pt_terminal_font_size(void) { return font_size_pts; }

void pt_terminal_set_font_size(int pts) {
  pts = CLAMP(pts, PT_FONT_SIZE_MIN, PT_FONT_SIZE_MAX);
  if (pts == font_size_pts) return;
  font_size_pts = pts;
  for (GSList *l = live_terminals; l != NULL; l = l->next) {
    PtTerminal *t = l->data;
    pango_font_description_set_size(t->font_desc, pts * PANGO_SCALE);
    pango_layout_set_font_description(t->layout, t->font_desc);
    measure_font(t);
    /* size_allocate re-derives cols/rows from the new cell metrics and
     * resizes the PTY + vt (reflow). */
    gtk_widget_queue_resize(GTK_WIDGET(t));
    gtk_widget_queue_draw(GTK_WIDGET(t));
  }
}

/* ---- boilerplate ---- */
static void pt_terminal_dispose(GObject *obj) {
  PtTerminal *t = PT_TERMINAL(obj);
  live_terminals = g_slist_remove(live_terminals, t);
  g_clear_pointer(&t->core, pt_term_core_free);
  g_clear_object(&t->layout);
  g_clear_pointer(&t->font_desc, pango_font_description_free);
  g_clear_pointer(&t->start_cwd, g_free);
  g_clear_pointer(&t->env, g_strfreev);
  g_clear_pointer(&t->last_command, g_free);
  G_OBJECT_CLASS(pt_terminal_parent_class)->dispose(obj);
}

static void pt_terminal_class_init(PtTerminalClass *klass) {
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  oc->dispose = pt_terminal_dispose;
  wc->snapshot = pt_terminal_snapshot;
  wc->size_allocate = pt_terminal_size_allocate;
  gtk_widget_class_set_css_name(wc, "pt-terminal");
  signals[SIG_EXITED] = g_signal_new("exited", PT_TYPE_TERMINAL,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
  signals[SIG_TITLE_CHANGED] = g_signal_new("title-changed", PT_TYPE_TERMINAL,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_ACTIVITY] = g_signal_new("activity", PT_TYPE_TERMINAL,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_COMMAND_CHANGED] = g_signal_new("command-changed", PT_TYPE_TERMINAL,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void pt_terminal_init(PtTerminal *t) {
  gtk_widget_set_focusable(GTK_WIDGET(t), TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(t), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(t), TRUE);
  t->font_desc = pango_font_description_from_string(PT_FONT_FAMILY);
  pango_font_description_set_size(t->font_desc, font_size_pts * PANGO_SCALE);
  t->layout = gtk_widget_create_pango_layout(GTK_WIDGET(t), NULL);
  pango_layout_set_font_description(t->layout, t->font_desc);
  live_terminals = g_slist_prepend(live_terminals, t);

  GtkEventController *key = gtk_event_controller_key_new();
  g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), t);
  gtk_widget_add_controller(GTK_WIDGET(t), key);

  GtkEventController *scroll =
      gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), t);
  gtk_widget_add_controller(GTK_WIDGET(t), scroll);

  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click, "pressed", G_CALLBACK(on_click_pressed), t);
  g_signal_connect(click, "released", G_CALLBACK(on_click_released), t);
  gtk_widget_add_controller(GTK_WIDGET(t), GTK_EVENT_CONTROLLER(click));

  GtkGesture *drag = gtk_gesture_drag_new();
  g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), t);
  gtk_widget_add_controller(GTK_WIDGET(t), GTK_EVENT_CONTROLLER(drag));

  GtkEventController *focus = gtk_event_controller_focus_new();
  g_signal_connect(focus, "enter", G_CALLBACK(on_focus_enter), t);
  g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), t);
  gtk_widget_add_controller(GTK_WIDGET(t), focus);
}

void pt_terminal_set_default_env(const char *const *env_pairs) {
  g_clear_pointer(&default_env, g_strfreev);
  if (env_pairs != NULL) default_env = g_strdupv((char **)env_pairs);
}

GtkWidget *pt_terminal_new_full(const char *cwd, const char *const *env_pairs) {
  PtTerminal *t = g_object_new(PT_TYPE_TERMINAL, NULL);
  t->start_cwd = g_strdup(cwd != NULL ? cwd : g_get_home_dir());
  if (env_pairs != NULL) t->env = g_strdupv((char **)env_pairs);
  return GTK_WIDGET(t);
}

GtkWidget *pt_terminal_new(const char *cwd) {
  return pt_terminal_new_full(cwd, (const char *const *)default_env);
}
