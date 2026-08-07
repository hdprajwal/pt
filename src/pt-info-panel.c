#include "pt-info-panel.h"
#include "pt-accent.h"
#include "pt-rowlist.h"

#include <string.h>

/* Mirror of the sidebar rail on the other edge of the window. */
#define PT_INFO_PANEL_WIDTH 266

enum { SIG_OPEN_EDITOR, SIG_OPEN_FILES, SIG_COPY_PATH, SIG_REFRESH,
       SIG_USAGE_ENABLE, N_SIGNALS };
static guint signals[N_SIGNALS];

/* A limit window on screen: its name and numbers on one line, its bar under
 * them. Every one of these is built once and then shown, hidden and rewritten
 * in place — the panel refreshes twice a second to keep the countdowns moving,
 * and rebuilding a widget tree at that rate to redraw numbers that change once
 * every two minutes would be pure waste. */
typedef struct {
  GtkWidget *row;    /* the pair: header line and bar */
  GtkWidget *label, *pct, *reset;
  GtkWidget *bar;
} PtUsageBarUI;

/* One per limit window the model can hold, plus the context bar at the end. */
#define PT_INFO_BAR_COUNT (PT_USAGE_MAX_WINDOWS + 1)

struct _PtInfoPanel {
  GtkWidget parent_instance;
  GtkWidget *box;                  /* vertical: info, agent usage, git */
  GtkWidget *dot, *shell, *pid, *dir, *zed;
  /* ---- agent usage ----
   * Hidden whole while no agent is running, which is most of the time. */
  GtkWidget *usage_sect;
  GtkWidget *agent_name, *agent_plan, *agent_hit;
  PtUsageBarUI bars[PT_INFO_BAR_COUNT];
  GtkWidget *usage_src;            /* where the numbers came from, and when */
  GtkWidget *usage_err_row, *usage_err;
  GtkWidget *usage_optin_row, *usage_optin_btn;
  /* The git section's header row IS the branch line: icon, branch, count,
   * ahead/behind — or the icon and `not_repo` when there is no repo. */
  GtkWidget *branch_icon, *branch, *count, *ab, *not_repo;
  GtkWidget *scroller;
  /* The file rows. The row list holds the PtGitFile* array they were built
   * from — a reference to it, not a copy — and skips the rebuild when the list
   * has not moved. */
  PtRowList *files;
  int accent;                      /* -1 until the first set_info */
};

G_DEFINE_FINAL_TYPE(PtInfoPanel, pt_info_panel, GTK_TYPE_WIDGET)

/* ---------- callbacks ---------- */
static void on_refresh_clicked(GtkButton *btn, gpointer user) {
  (void)btn;
  g_signal_emit(PT_INFO_PANEL(user), signals[SIG_REFRESH], 0);
}

static void on_zed_clicked(GtkButton *btn, gpointer user) {
  (void)btn;
  g_signal_emit(PT_INFO_PANEL(user), signals[SIG_OPEN_EDITOR], 0);
}

static void on_files_clicked(GtkButton *btn, gpointer user) {
  (void)btn;
  g_signal_emit(PT_INFO_PANEL(user), signals[SIG_OPEN_FILES], 0);
}

static void on_copy_clicked(GtkButton *btn, gpointer user) {
  (void)btn;
  g_signal_emit(PT_INFO_PANEL(user), signals[SIG_COPY_PATH], 0);
}

static void on_usage_enable_clicked(GtkButton *btn, gpointer user) {
  (void)btn;
  g_signal_emit(PT_INFO_PANEL(user), signals[SIG_USAGE_ENABLE], 0);
}

/* ---------- file list ---------- */
/* M and R are edits, A additions, D removals, ?? untracked; anything else
 * (a typechange, a half-resolved conflict) falls back to dim rather than
 * borrowing a colour that would misreport it. */
static const char *status_class(const char *xy) {
  switch (xy[0]) {
    case 'M': case 'R': return "m";
    case 'A':           return "a";
    case 'D':           return "d";
    default:            return "u";
  }
}

/* One file row: status, directory, name, the two line counts. Nothing here is
 * clickable, so the row carries no gesture. */
static GtkWidget *build_file_row(gpointer items, guint idx, gpointer u) {
  (void)u;
  const PtGitFile *f = g_ptr_array_index((GPtrArray *)items, idx);
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(row, "pt-info-file");

  GtkWidget *st = gtk_label_new(f->xy);
  gtk_widget_add_css_class(st, "pt-info-st");
  gtk_widget_add_css_class(st, status_class(f->xy));
  gtk_label_set_xalign(GTK_LABEL(st), 0.5f);
  gtk_box_append(GTK_BOX(row), st);

  const char *slash = strrchr(f->path, '/');
  if (slash != NULL) {
    char *dir = g_strndup(f->path, (gsize)(slash - f->path) + 1);
    GtkWidget *dl = gtk_label_new(dir);
    g_free(dir);
    gtk_widget_add_css_class(dl, "pt-info-fdir");
    gtk_label_set_ellipsize(GTK_LABEL(dl), PANGO_ELLIPSIZE_START);
    gtk_box_append(GTK_BOX(row), dl);
  }
  /* The name takes the slack and ellipsizes: the counts must keep their
   * place at the right edge of a 266px rail, whatever the path length. */
  GtkWidget *name = gtk_label_new(slash != NULL ? slash + 1 : f->path);
  gtk_widget_add_css_class(name, "pt-info-fname");
  gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_hexpand(name, TRUE);
  gtk_box_append(GTK_BOX(row), name);

  /* Untracked files never reach the diff, and binary ones report no lines:
   * both leave the counts off entirely rather than printing a fake 0. */
  if (f->add >= 0 && f->del >= 0) {
    char buf[24];
    g_snprintf(buf, sizeof(buf), "+%d", f->add);
    GtkWidget *add = gtk_label_new(buf);
    gtk_widget_add_css_class(add, "pt-info-add");
    gtk_box_append(GTK_BOX(row), add);
    g_snprintf(buf, sizeof(buf), "−%d", f->del);
    GtkWidget *del = gtk_label_new(buf);
    gtk_widget_add_css_class(del, "pt-info-del");
    gtk_box_append(GTK_BOX(row), del);
  }
  return row;
}

static gboolean files_equal(gpointer ap, guint na, gpointer bp, guint nb,
                            gpointer u) {
  (void)u;
  if (na != nb) return FALSE;
  for (guint i = 0; i < na; i++) {
    const PtGitFile *a = g_ptr_array_index((GPtrArray *)ap, i);
    const PtGitFile *b = g_ptr_array_index((GPtrArray *)bp, i);
    /* Counts too: they arrive one poll behind the paths, and skipping the
     * rebuild for them would leave every row blank forever. */
    if (strcmp(a->xy, b->xy) != 0 || g_strcmp0(a->path, b->path) != 0 ||
        a->add != b->add || a->del != b->del)
      return FALSE;
  }
  return TRUE;
}

/* ---------- public API ---------- */
void pt_info_panel_set_info(PtInfoPanel *ip, const char *shell, int pid,
                            const char *dir, int accent) {
  gtk_label_set_text(GTK_LABEL(ip->shell), shell != NULL ? shell : "");
  char buf[32] = "";
  if (pid > 0) g_snprintf(buf, sizeof(buf), "pid %d", pid);
  gtk_label_set_text(GTK_LABEL(ip->pid), buf);
  gtk_label_set_text(GTK_LABEL(ip->dir), dir != NULL ? dir : "");
  if (accent != ip->accent) {
    pt_accent_set_class(ip->dot, accent);
    ip->accent = accent;
  }
}

/* The window refreshes this twice a second, so the file rows are rebuilt only
 * when the list itself moved — same reasoning as the sidebar's row dedupe. */
void pt_info_panel_set_git(PtInfoPanel *ip, const PtGitStatus *st,
                           gboolean is_repo, GPtrArray *files) {
  gtk_widget_set_visible(ip->branch, is_repo);
  gtk_widget_set_visible(ip->scroller, is_repo);
  gtk_widget_set_visible(ip->not_repo, !is_repo);
  /* The icon stays put in both states and only drops to the ghost tone. */
  if (is_repo) gtk_widget_remove_css_class(ip->branch_icon, "empty");
  else         gtk_widget_add_css_class(ip->branch_icon, "empty");

  char buf[32] = "";
  if (is_repo && st != NULL && st->changed > 0)
    g_snprintf(buf, sizeof(buf), "%d", st->changed);
  gtk_label_set_text(GTK_LABEL(ip->count), buf);
  gtk_widget_set_visible(ip->count, buf[0] != '\0');

  char ab[32] = "";
  if (is_repo && st != NULL) {
    gtk_label_set_text(GTK_LABEL(ip->branch), st->branch);
    if (st->ahead > 0 && st->behind > 0)
      g_snprintf(ab, sizeof(ab), "↑%d ↓%d", st->ahead, st->behind);
    else if (st->ahead > 0)  g_snprintf(ab, sizeof(ab), "↑%d", st->ahead);
    else if (st->behind > 0) g_snprintf(ab, sizeof(ab), "↓%d", st->behind);
    gtk_label_set_text(GTK_LABEL(ip->ab), ab);
  }
  gtk_widget_set_visible(ip->ab, ab[0] != '\0');

  if (!is_repo) files = NULL;
  /* A reference, not a copy: git updates always deliver a fresh array, so the
   * one shown here is never mutated behind the panel's back. The row list drops
   * this reference again when the list turns out not to have moved — including
   * when it is handed the very array already on screen. */
  pt_rowlist_set(ip->files, files != NULL ? g_ptr_array_ref(files) : NULL,
                 files != NULL ? files->len : 0, build_file_row, files_equal,
                 ip, (GDestroyNotify)g_ptr_array_unref);
}

void pt_info_panel_set_has_zed(PtInfoPanel *ip, gboolean has_zed) {
  gtk_widget_set_visible(ip->zed, has_zed);
}

/* ---------- agent usage ---------- */
/* Only when it moves: GTK invalidates a widget's style on every add and every
 * remove, whether or not the class was there, and this runs on the panel's
 * twice-a-second refresh. Same reasoning as pt_accent_set_class. */
static void set_class(GtkWidget *w, const char *cls, gboolean on) {
  if (gtk_widget_has_css_class(w, cls) == on) return;
  if (on) gtk_widget_add_css_class(w, cls);
  else    gtk_widget_remove_css_class(w, cls);
}

/* The bar's colour is the only thing that says "this is getting tight", so it
 * is the one piece of state here not spelled in words. Below three quarters it
 * carries the project's accent, like the dot at the top of the panel. */
static void bar_severity(GtkWidget *bar, double pct, int accent) {
  set_class(bar, "crit", pct >= 90.0);
  set_class(bar, "warn", pct >= 75.0 && pct < 90.0);
  pt_accent_set_class(bar, accent);
}

/* `resets_at` 0 means the source did not say when the window turns over, which
 * is not the same as "it resets now" — the countdown is simply left off. */
static void set_bar(PtUsageBarUI *b, const char *label, double pct,
                    gint64 resets_at, gint64 now, int accent) {
  gtk_widget_set_visible(b->row, TRUE);
  gtk_label_set_text(GTK_LABEL(b->label), label);

  char buf[16];
  g_snprintf(buf, sizeof(buf), "%.0f%%", pct);
  gtk_label_set_text(GTK_LABEL(b->pct), buf);

  char *left = resets_at > 0 ? pt_usage_format_duration(resets_at - now) : NULL;
  gtk_label_set_text(GTK_LABEL(b->reset), left != NULL ? left : "");
  gtk_widget_set_visible(b->reset, left != NULL);

  /* The row is three short pieces on a 266px rail, so the sentence the issue
   * asks for lives in the tooltip and the row keeps the numbers. */
  char *tip = left != NULL
                  ? g_strdup_printf("%s: %.0f%% used, resets in %s", label,
                                    pct, left)
                  : g_strdup_printf("%s: %.0f%% used", label, pct);
  gtk_widget_set_tooltip_text(b->row, tip);
  g_free(tip);
  g_free(left);

  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(b->bar), pct / 100.0);
  bar_severity(b->bar, pct, accent);
}

void pt_info_panel_set_usage(PtInfoPanel *ip, const PtAgentView *v,
                             gint64 now) {
  /* No agent: the section goes away entirely rather than sitting there empty.
   * Note this is driven by the view's kind alone — the monitor keeps the last
   * reading across the moment an agent disappears, so a tab switch hides this
   * and shows it again with its numbers intact instead of blanking them. */
  gtk_widget_set_visible(ip->usage_sect, v->kind != PT_AGENT_NONE);
  if (v->kind == PT_AGENT_NONE) return;

  gtk_label_set_text(GTK_LABEL(ip->agent_name), pt_agent_label(v->kind));

  const PtUsage *u = v->usage;
  const char *plan = u != NULL ? u->plan : "";
  gtk_label_set_text(GTK_LABEL(ip->agent_plan), plan);
  gtk_widget_set_visible(ip->agent_plan, plan[0] != '\0');
  gtk_widget_set_visible(ip->agent_hit, u != NULL && u->limit_hit);

  /* The opt-in and the bars are mutually exclusive: until the user says yes
   * there is nothing to draw, because nothing has been fetched. */
  gtk_widget_set_visible(ip->usage_optin_row, v->needs_optin);

  int shown = 0;
  if (u != NULL && !v->needs_optin) {
    for (int i = 0; i < u->n_windows && shown < PT_INFO_BAR_COUNT; i++)
      set_bar(&ip->bars[shown++], u->windows[i].label, u->windows[i].percent,
              u->windows[i].resets_at, now, ip->accent);
    int ctx = pt_usage_context_percent(u);
    if (ctx >= 0 && shown < PT_INFO_BAR_COUNT)
      set_bar(&ip->bars[shown++], "context", ctx, 0, now, ip->accent);
  }
  for (int i = shown; i < PT_INFO_BAR_COUNT; i++)
    gtk_widget_set_visible(ip->bars[i].row, FALSE);

  /* The source line doubles as the staleness line: a number with no age on it
   * looks live whether or not it is. */
  char *age = u != NULL ? pt_usage_format_age(u->fetched_at, now) : NULL;
  char *src = NULL;
  if (v->busy && u == NULL)   src = g_strdup("checking…");
  else if (u != NULL)         src = g_strdup_printf("%s · %s", u->source,
                                                    age != NULL ? age : "");
  gtk_label_set_text(GTK_LABEL(ip->usage_src), src != NULL ? src : "");
  gtk_widget_set_visible(ip->usage_src, src != NULL && !v->needs_optin);
  g_free(src);
  g_free(age);

  /* Under the numbers, not instead of them: a failed refresh does not make a
   * two-minute-old reading wrong, so the old bars stay and this explains why
   * they are not moving. */
  gboolean has_err = v->error != NULL && !v->needs_optin;
  gtk_widget_set_visible(ip->usage_err_row, has_err);
  if (has_err) gtk_label_set_text(GTK_LABEL(ip->usage_err), v->error);
}

/* ---------- GObject ---------- */
static void pt_info_panel_dispose(GObject *obj) {
  PtInfoPanel *ip = PT_INFO_PANEL(obj);
  g_clear_pointer(&ip->box, gtk_widget_unparent);
  g_clear_object(&ip->files);   /* drops the file array's reference too */
  G_OBJECT_CLASS(pt_info_panel_parent_class)->dispose(obj);
}

/* Same fix as the sidebar: a size request only raises the minimum, and the
 * wrapping directory label reports its full text width as natural, which would
 * let a deep path widen the panel and steal that width from the terminal.
 * Reporting minimum == natural pins the rail. */
static void pt_info_panel_measure(GtkWidget *widget, GtkOrientation orientation,
                                  int for_size, int *minimum, int *natural,
                                  int *minimum_baseline,
                                  int *natural_baseline) {
  (void)for_size;
  PtInfoPanel *ip = PT_INFO_PANEL(widget);
  *minimum_baseline = *natural_baseline = -1;
  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    *minimum = *natural = PT_INFO_PANEL_WIDTH;
    return;
  }
  if (ip->box == NULL) { *minimum = *natural = 0; return; }
  gtk_widget_measure(ip->box, orientation, PT_INFO_PANEL_WIDTH,
                     minimum, natural, minimum_baseline, natural_baseline);
}

static void pt_info_panel_size_allocate(GtkWidget *widget, int width,
                                        int height, int baseline) {
  PtInfoPanel *ip = PT_INFO_PANEL(widget);
  if (ip->box != NULL)
    gtk_widget_allocate(ip->box, width, height, baseline, NULL);
}

static void pt_info_panel_class_init(PtInfoPanelClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_info_panel_dispose;
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  wc->measure = pt_info_panel_measure;
  wc->size_allocate = pt_info_panel_size_allocate;
  signals[SIG_OPEN_EDITOR] = g_signal_new("open-editor", PT_TYPE_INFO_PANEL,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_OPEN_FILES] = g_signal_new("open-files", PT_TYPE_INFO_PANEL,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_COPY_PATH] = g_signal_new("copy-path", PT_TYPE_INFO_PANEL,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_REFRESH] = g_signal_new("refresh", PT_TYPE_INFO_PANEL,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_USAGE_ENABLE] = g_signal_new("usage-enable", PT_TYPE_INFO_PANEL,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static GtkWidget *section_head(const char *text) {
  GtkWidget *l = gtk_label_new(text);
  gtk_widget_add_css_class(l, "pt-info-head");
  gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
  return l;
}

/* `icon` NULL keeps the plain text button; a name puts the symbolic icon and
 * the label side by side in one child box. */
static GtkWidget *action_button(PtInfoPanel *ip, const char *icon,
                                const char *label, GCallback cb) {
  GtkWidget *b;
  if (icon == NULL) {
    b = gtk_button_new_with_label(label);
  } else {
    b = gtk_button_new();
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *img = gtk_image_new_from_icon_name(icon);
    gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), img);
    gtk_box_append(GTK_BOX(row), gtk_label_new(label));
    gtk_button_set_child(GTK_BUTTON(b), row);
  }
  gtk_widget_add_css_class(b, "flat");
  gtk_widget_add_css_class(b, "pt-info-btn");
  g_signal_connect(b, "clicked", cb, ip);
  return b;
}

/* One limit window's two lines, built empty. Hidden until something fills it,
 * so the panel is never briefly a stack of blank bars. */
static void build_usage_bar(PtUsageBarUI *b, GtkWidget *parent) {
  b->row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class(b->row, "pt-usage-row");
  gtk_widget_set_visible(b->row, FALSE);

  GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  b->label = gtk_label_new("");
  gtk_widget_add_css_class(b->label, "pt-usage-name");
  gtk_label_set_xalign(GTK_LABEL(b->label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(b->label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(b->label, TRUE);
  gtk_box_append(GTK_BOX(head), b->label);
  b->pct = gtk_label_new("");
  gtk_widget_add_css_class(b->pct, "pt-usage-pct");
  gtk_box_append(GTK_BOX(head), b->pct);
  b->reset = gtk_label_new("");
  gtk_widget_add_css_class(b->reset, "pt-usage-reset");
  gtk_box_append(GTK_BOX(head), b->reset);
  gtk_box_append(GTK_BOX(b->row), head);

  /* GtkProgressBar rather than a hand-measured box: its trough and progress
   * nodes take a flat 3px style directly, and the fraction is the one number
   * this has to express. */
  b->bar = gtk_progress_bar_new();
  gtk_widget_add_css_class(b->bar, "pt-usage-bar");
  gtk_box_append(GTK_BOX(b->row), b->bar);
  gtk_box_append(GTK_BOX(parent), b->row);
}

static void build_usage_section(PtInfoPanel *ip) {
  ip->usage_sect = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(ip->usage_sect, "pt-info-sect");
  gtk_widget_add_css_class(ip->usage_sect, "agent");
  gtk_widget_set_visible(ip->usage_sect, FALSE);
  gtk_box_append(GTK_BOX(ip->usage_sect), section_head("AGENT USAGE"));

  /* The agent's name is the section's identity line: no dot, because the one
   * at the top of the panel already speaks for the pane. */
  GtkWidget *ident = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  ip->agent_name = gtk_label_new("");
  gtk_widget_add_css_class(ip->agent_name, "pt-usage-agent");
  gtk_label_set_xalign(GTK_LABEL(ip->agent_name), 0.0f);
  gtk_box_append(GTK_BOX(ident), ip->agent_name);
  ip->agent_plan = gtk_label_new("");
  gtk_widget_add_css_class(ip->agent_plan, "pt-usage-plan");
  gtk_widget_set_valign(ip->agent_plan, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(ident), ip->agent_plan);
  ip->agent_hit = gtk_label_new("limit reached");
  gtk_widget_add_css_class(ip->agent_hit, "pt-usage-hit");
  gtk_widget_set_hexpand(ip->agent_hit, TRUE);
  gtk_widget_set_halign(ip->agent_hit, GTK_ALIGN_END);
  gtk_widget_set_visible(ip->agent_hit, FALSE);
  gtk_box_append(GTK_BOX(ident), ip->agent_hit);
  gtk_box_append(GTK_BOX(ip->usage_sect), ident);

  GtkWidget *bars = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
  gtk_widget_add_css_class(bars, "pt-usage-bars");
  for (int i = 0; i < PT_INFO_BAR_COUNT; i++)
    build_usage_bar(&ip->bars[i], bars);
  gtk_box_append(GTK_BOX(ip->usage_sect), bars);

  ip->usage_src = gtk_label_new("");
  gtk_widget_add_css_class(ip->usage_src, "pt-usage-src");
  gtk_label_set_xalign(GTK_LABEL(ip->usage_src), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(ip->usage_src), PANGO_ELLIPSIZE_END);
  gtk_widget_set_visible(ip->usage_src, FALSE);
  gtk_box_append(GTK_BOX(ip->usage_sect), ip->usage_src);

  /* Error and retry, kept as one row so the button can never appear without
   * the reason for it. */
  ip->usage_err_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_widget_set_visible(ip->usage_err_row, FALSE);
  ip->usage_err = gtk_label_new("");
  gtk_widget_add_css_class(ip->usage_err, "pt-usage-err");
  gtk_label_set_xalign(GTK_LABEL(ip->usage_err), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(ip->usage_err), TRUE);
  gtk_box_append(GTK_BOX(ip->usage_err_row), ip->usage_err);
  GtkWidget *retry = action_button(ip, NULL, "Retry",
                                   G_CALLBACK(on_refresh_clicked));
  gtk_widget_set_halign(retry, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(ip->usage_err_row), retry);
  gtk_box_append(GTK_BOX(ip->usage_sect), ip->usage_err_row);

  /* The opt-in. Claude Code stores no usage locally, so these numbers can only
   * come from a request to Anthropic carrying the user's token — which is
   * theirs to allow, so it says what it will do and waits to be pressed. */
  ip->usage_optin_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_visible(ip->usage_optin_row, FALSE);
  GtkWidget *why = gtk_label_new(
      "Reads your plan limits from Anthropic, using the login Claude Code "
      "already stored.");
  gtk_widget_add_css_class(why, "pt-usage-why");
  gtk_label_set_xalign(GTK_LABEL(why), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(why), TRUE);
  gtk_box_append(GTK_BOX(ip->usage_optin_row), why);
  ip->usage_optin_btn = action_button(ip, NULL, "Turn on",
                                      G_CALLBACK(on_usage_enable_clicked));
  gtk_widget_set_halign(ip->usage_optin_btn, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(ip->usage_optin_row), ip->usage_optin_btn);
  gtk_box_append(GTK_BOX(ip->usage_sect), ip->usage_optin_row);
}

static void pt_info_panel_init(PtInfoPanel *ip) {
  gtk_widget_add_css_class(GTK_WIDGET(ip), "pt-infopanel");
  gtk_widget_set_size_request(GTK_WIDGET(ip), PT_INFO_PANEL_WIDTH, -1);
  ip->accent = -1;

  ip->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent(ip->box, GTK_WIDGET(ip));

  /* ---- INFO ---- */
  GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(info, "pt-info-sect");

  GtkWidget *info_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(GTK_BOX(info_head), section_head("INFO"));
  GtkWidget *refresh = gtk_button_new_with_label("↻");
  gtk_widget_add_css_class(refresh, "flat");
  gtk_widget_add_css_class(refresh, "pt-info-refresh");
  gtk_widget_set_hexpand(refresh, TRUE);
  gtk_widget_set_halign(refresh, GTK_ALIGN_END);
  gtk_widget_set_valign(refresh, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(refresh, "Refresh");
  g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh_clicked), ip);
  gtk_box_append(GTK_BOX(info_head), refresh);
  gtk_box_append(GTK_BOX(info), info_head);

  GtkWidget *ident = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  ip->dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(ip->dot, "pt-dot");
  gtk_widget_add_css_class(ip->dot, "pt-dot-7");
  gtk_widget_set_valign(ip->dot, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(ident), ip->dot);
  ip->shell = gtk_label_new("");
  gtk_widget_add_css_class(ip->shell, "pt-info-shell");
  gtk_label_set_ellipsize(GTK_LABEL(ip->shell), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(ident), ip->shell);
  ip->pid = gtk_label_new("");
  gtk_widget_add_css_class(ip->pid, "pt-info-pid");
  gtk_box_append(GTK_BOX(ident), ip->pid);
  gtk_box_append(GTK_BOX(info), ident);

  ip->dir = gtk_label_new("");
  gtk_widget_add_css_class(ip->dir, "pt-info-path");
  gtk_label_set_xalign(GTK_LABEL(ip->dir), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(ip->dir), TRUE);
  /* A path has no spaces to break on, so CHAR is the only mode that wraps it
   * instead of forcing the panel wider. */
  gtk_label_set_wrap_mode(GTK_LABEL(ip->dir), PANGO_WRAP_WORD_CHAR);
  gtk_box_append(GTK_BOX(info), ip->dir);

  GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(actions, GTK_ALIGN_START);
  ip->zed = action_button(ip, "pt-zed-symbolic", "Zed",
                          G_CALLBACK(on_zed_clicked));
  gtk_box_append(GTK_BOX(actions), ip->zed);
  gtk_box_append(GTK_BOX(actions),
                 action_button(ip, NULL, "⊞ Files",
                               G_CALLBACK(on_files_clicked)));
  gtk_box_append(GTK_BOX(actions),
                 action_button(ip, NULL, "⧉ Copy",
                               G_CALLBACK(on_copy_clicked)));
  gtk_box_append(GTK_BOX(info), actions);
  gtk_box_append(GTK_BOX(ip->box), info);

  /* ---- AGENT USAGE ----
   * Above git and below the pane's identity: it is about what is running in
   * the pane, and the git section wants the bottom of the rail because its
   * file list is the one thing here that scrolls. */
  build_usage_section(ip);
  gtk_box_append(GTK_BOX(ip->box), ip->usage_sect);

  /* ---- GIT ---- */
  GtkWidget *git = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(git, "pt-info-sect");
  gtk_widget_add_css_class(git, "git");
  gtk_widget_set_vexpand(git, TRUE);

  /* The branch line doubles as the section header: no "GIT" caption above it,
   * the icon says which section this is. */
  GtkWidget *git_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  ip->branch_icon = gtk_image_new_from_icon_name("pt-git-branch-symbolic");
  gtk_widget_add_css_class(ip->branch_icon, "pt-info-branch-icon");
  gtk_widget_set_valign(ip->branch_icon, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(git_head), ip->branch_icon);
  ip->branch = gtk_label_new("");
  gtk_widget_add_css_class(ip->branch, "pt-info-branch");
  gtk_label_set_ellipsize(GTK_LABEL(ip->branch), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_append(GTK_BOX(git_head), ip->branch);
  ip->count = gtk_label_new("");
  gtk_widget_add_css_class(ip->count, "pt-info-count");
  gtk_widget_set_visible(ip->count, FALSE);
  gtk_box_append(GTK_BOX(git_head), ip->count);
  ip->ab = gtk_label_new("");
  gtk_widget_add_css_class(ip->ab, "pt-info-ab");
  gtk_widget_set_visible(ip->ab, FALSE);
  gtk_box_append(GTK_BOX(git_head), ip->ab);
  ip->not_repo = gtk_label_new("no git repo found");
  gtk_widget_add_css_class(ip->not_repo, "pt-info-empty");
  gtk_widget_set_visible(ip->not_repo, FALSE);
  gtk_box_append(GTK_BOX(git_head), ip->not_repo);
  gtk_box_append(GTK_BOX(git), git_head);

  /* EXTERNAL horizontally: no h-scrollbar, and the file rows contribute
   * nothing to the panel's width request (see the measure override). */
  ip->scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ip->scroller),
                                 GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(ip->scroller, TRUE);
  GtkWidget *files_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(ip->scroller), files_box);
  ip->files = pt_rowlist_new(GTK_BOX(files_box));
  gtk_box_append(GTK_BOX(git), ip->scroller);
  gtk_box_append(GTK_BOX(ip->box), git);
}

GtkWidget *pt_info_panel_new(void) {
  return g_object_new(PT_TYPE_INFO_PANEL, NULL);
}
