#include "pt-statusline.h"
#include "pt-session.h"   /* PT_ACCENT_COUNT */

/* Track width in px. The fill is sized as a fraction of this, so the constant
 * has to agree with `.pt-progress-track { min-width }` in style.css. */
#define PT_PROGRESS_W 120
#define PT_PROGRESS_H 3

/* Right-hand hint. ASCII "^" reads as Ctrl in the reference mockup; the two
 * glyphs are alt+tab, which has no ASCII spelling that stays legible. */
#define PT_STATUS_HINT \
  "^K palette · ^1…9 projects · ⌥⇥ shells · ^T new shell"

struct _PtStatusline {
  GtkWidget parent_instance;
  GtkWidget *box;
  GtkWidget *state;   /* "● running" / "✓ exit 0" / "✗ exit 2" */
  GtkWidget *track;   /* progress groove; hidden with no parsed progress */
  GtkWidget *fill;    /* accent-coloured child of the track */
  GtkWidget *task;    /* "cargo  128/214"; hidden with the track */
  GtkWidget *hint;    /* end: muted keybinding hint */
};

G_DEFINE_FINAL_TYPE(PtStatusline, pt_statusline, GTK_TYPE_WIDGET)

/* Exactly one pt-aN class survives on `wdg`. Accents outside 0..5 are folded
 * back into range rather than dropped, so a bad index still colours something.*/
static void set_accent_class(GtkWidget *wdg, int accent) {
  int a = accent % PT_ACCENT_COUNT;
  if (a < 0) a += PT_ACCENT_COUNT;
  for (int i = 0; i < PT_ACCENT_COUNT; i++) {
    char c[8];
    g_snprintf(c, sizeof(c), "pt-a%d", i);
    gtk_widget_remove_css_class(wdg, c);
  }
  char c[8];
  g_snprintf(c, sizeof(c), "pt-a%d", a);
  gtk_widget_add_css_class(wdg, c);
}

/* Exactly one of run/ok/err survives on the state label. */
static void set_state_class(GtkWidget *wdg, const char *keep) {
  static const char *const all[] = { "run", "ok", "err" };
  for (guint i = 0; i < G_N_ELEMENTS(all); i++)
    if (g_strcmp0(all[i], keep) != 0)
      gtk_widget_remove_css_class(wdg, all[i]);
  gtk_widget_add_css_class(wdg, keep);
}

void pt_statusline_update(PtStatusline *sl, gboolean running, int last_exit,
                          const PtProgress *progress, const char *task_label,
                          int accent) {
  g_return_if_fail(PT_IS_STATUSLINE(sl));

  if (running) {
    gtk_label_set_text(GTK_LABEL(sl->state), "● running");
    set_state_class(sl->state, "run");
  } else if (last_exit > 0) {
    char *txt = g_strdup_printf("✗ exit %d", last_exit);
    gtk_label_set_text(GTK_LABEL(sl->state), txt);
    g_free(txt);
    set_state_class(sl->state, "err");
  } else if (last_exit == 0) {
    gtk_label_set_text(GTK_LABEL(sl->state), "✓ exit 0");
    set_state_class(sl->state, "ok");
  } else {
    /* nothing has ever reported an exit status for this pane */
    gtk_label_set_text(GTK_LABEL(sl->state), "✓");
    set_state_class(sl->state, "ok");
  }

  /* No parsed progress → no bar at all. */
  double fraction = -1.0;
  char *task_txt = NULL;
  if (progress != NULL) {
    const char *name = (task_label != NULL && task_label[0] != '\0')
                           ? task_label : NULL;
    if (progress->has_fraction && progress->total > 0) {
      fraction = progress->done / (double)progress->total;
      task_txt = name != NULL
          ? g_strdup_printf("%s  %d/%d", name, progress->done, progress->total)
          : g_strdup_printf("%d/%d", progress->done, progress->total);
    } else if (progress->has_percent) {
      fraction = progress->percent / 100.0;
      task_txt = name != NULL
          ? g_strdup_printf("%s  %d%%", name, progress->percent)
          : g_strdup_printf("%d%%", progress->percent);
    }
  }

  gboolean show = fraction >= 0.0;
  if (show) {
    fraction = CLAMP(fraction, 0.0, 1.0);
    /* Geometry, not styling: the groove is a fixed 120px and the fill spans
     * its completed share of it. */
    gtk_widget_set_size_request(sl->fill, (int)(fraction * PT_PROGRESS_W),
                                PT_PROGRESS_H);
    set_accent_class(sl->fill, accent);
    gtk_label_set_text(GTK_LABEL(sl->task), task_txt);
  }
  g_free(task_txt);
  gtk_widget_set_visible(sl->track, show);
  gtk_widget_set_visible(sl->task, show);
}

static void pt_statusline_dispose(GObject *obj) {
  PtStatusline *sl = PT_STATUSLINE(obj);
  g_clear_pointer(&sl->box, gtk_widget_unparent);
  G_OBJECT_CLASS(pt_statusline_parent_class)->dispose(obj);
}

static void pt_statusline_class_init(PtStatuslineClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_statusline_dispose;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass),
                                           GTK_TYPE_BIN_LAYOUT);
}

static void pt_statusline_init(PtStatusline *sl) {
  gtk_widget_add_css_class(GTK_WIDGET(sl), "pt-statusbar");
  sl->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_parent(sl->box, GTK_WIDGET(sl));

  sl->state = gtk_label_new("✓");
  gtk_widget_add_css_class(sl->state, "pt-run-state");
  gtk_widget_add_css_class(sl->state, "ok");
  gtk_label_set_xalign(GTK_LABEL(sl->state), 0.0f);
  gtk_box_append(GTK_BOX(sl->box), sl->state);

  sl->track = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(sl->track, "pt-progress-track");
  gtk_widget_set_size_request(sl->track, PT_PROGRESS_W, PT_PROGRESS_H);
  gtk_widget_set_valign(sl->track, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(sl->track, FALSE);
  sl->fill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(sl->fill, "pt-progress-fill");
  /* left-anchored so the fill grows rightwards out of the groove's start */
  gtk_widget_set_halign(sl->fill, GTK_ALIGN_START);
  gtk_widget_set_size_request(sl->fill, 0, PT_PROGRESS_H);
  gtk_box_append(GTK_BOX(sl->track), sl->fill);
  gtk_box_append(GTK_BOX(sl->box), sl->track);

  sl->task = gtk_label_new("");
  gtk_widget_add_css_class(sl->task, "pt-task-label");
  gtk_label_set_xalign(GTK_LABEL(sl->task), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(sl->task), PANGO_ELLIPSIZE_END);
  gtk_widget_set_visible(sl->task, FALSE);
  gtk_box_append(GTK_BOX(sl->box), sl->task);

  GtkWidget *spacer = gtk_label_new(NULL);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(sl->box), spacer);

  /* the hint is fixed copy and must never clip; the task label yields first */
  sl->hint = gtk_label_new(PT_STATUS_HINT);
  gtk_widget_add_css_class(sl->hint, "pt-status-hint");
  gtk_label_set_ellipsize(GTK_LABEL(sl->hint), PANGO_ELLIPSIZE_NONE);
  gtk_widget_set_halign(sl->hint, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(sl->box), sl->hint);
}

GtkWidget *pt_statusline_new(void) {
  return g_object_new(PT_TYPE_STATUSLINE, NULL);
}
