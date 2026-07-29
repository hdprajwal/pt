#include "pt-settings.h"

#include "pt-theme.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>

/* The dialog is deliberately tiny: appearance, theme, two font sizes, two
 * families. Layout and padding stay out of it — those are the app's opinion,
 * not a setting. Appearance sits above Theme because it scopes it: the list the
 * row underneath walks is only the themes of that appearance. */
#define PT_SETTINGS_WIDTH 560
#define ROW_APPEARANCE    0
#define ROW_THEME         1
#define ROW_FONT_SIZE     2
#define ROW_UI_FONT_SIZE  3
#define ROW_FONT_FAMILY   4
#define ROW_UI_FONT_FAMILY 5
#define N_ROWS            6

/* Display clamps. The parser accepts a wider range (PT_CONFIG_FONT_SIZE_*);
 * these are what the arrows will walk you to. */
#define FONT_SIZE_MIN    6
#define FONT_SIZE_MAX    32
#define UI_FONT_SIZE_MIN 9.0
#define UI_FONT_SIZE_MAX 20.0
#define UI_FONT_SIZE_STEP 0.5

enum { SIG_CHANGED, SIG_COMMITTED, SIG_REVERTED, SIG_CLOSED, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _PtSettings {
  GtkWidget parent_instance;
  GtkWidget *scrim;   /* sole child of the widget; .pt-palette-scrim */
  GtkWidget *panel;   /* .pt-settings */
  GtkWidget *list;    /* vertical box of the six rows, built once */
  GtkWidget *value_labels[N_ROWS];
  PtConfig *candidate;  /* live-edited copy; kept until the next open */
  /* The open-time theme list, already split by appearance: the whole list is
   * never wanted again, only the side the chosen appearance walks. */
  char **dark_themes;
  char **light_themes;
  /* Dialog-local and deliberately not persisted: the config surface stays at
   * `theme`, and the appearance shown is just what the selected theme is. */
  gboolean appearance_dark;
  char **mono_fams;     /* installed monospace families, sorted */
  char **all_fams;      /* all installed families, sorted */
  int selected;         /* row index */
  gboolean open;
};

G_DEFINE_FINAL_TYPE(PtSettings, pt_settings, GTK_TYPE_WIDGET)

/* ---------- installed families ---------- */
/* Collation is sorted on precomputed keys (a Schwartzian transform): qsort
 * runs the comparator O(n log n) times, and g_utf8_collate in there would
 * re-derive both sides' collation keys on every call — with hundreds of
 * installed families that is the slow part of opening the dialog. One
 * g_utf8_collate_key per name, strcmp in the comparator, keys freed after. */
typedef struct { char *key; char *name; } KeyedName;

static int cmp_keyed(const void *a, const void *b) {
  return strcmp(((const KeyedName *)a)->key, ((const KeyedName *)b)->key);
}

static void sort_families(GPtrArray *names) {
  if (names->len < 2) return;
  KeyedName *v = g_new(KeyedName, names->len);
  for (guint i = 0; i < names->len; i++) {
    v[i].name = g_ptr_array_index(names, i);
    v[i].key = g_utf8_collate_key(v[i].name, -1);
  }
  qsort(v, names->len, sizeof *v, cmp_keyed);
  for (guint i = 0; i < names->len; i++) {
    g_ptr_array_index(names, i) = v[i].name;
    g_free(v[i].key);
  }
  g_free(v);
}

/* Snapshot the font map into two sorted, NULL-terminated vectors. Pango owns
 * the PangoFontFamily objects; only the array itself is ours (g_free), so the
 * names have to be copied out. */
static void collect_families(PtSettings *s) {
  g_clear_pointer(&s->mono_fams, g_strfreev);
  g_clear_pointer(&s->all_fams, g_strfreev);

  PangoFontFamily **fams = NULL;
  int n = 0;
  pango_font_map_list_families(pango_cairo_font_map_get_default(), &fams, &n);

  GPtrArray *all = g_ptr_array_new();
  GPtrArray *mono = g_ptr_array_new();
  for (int i = 0; i < n; i++) {
    const char *name = pango_font_family_get_name(fams[i]);
    if (name == NULL || name[0] == '\0') continue;
    g_ptr_array_add(all, g_strdup(name));
    if (pango_font_family_is_monospace(fams[i]))
      g_ptr_array_add(mono, g_strdup(name));
  }
  g_free(fams);

  sort_families(all);
  sort_families(mono);
  g_ptr_array_add(all, NULL);
  g_ptr_array_add(mono, NULL);
  s->all_fams = (char **)g_ptr_array_free(all, FALSE);
  s->mono_fams = (char **)g_ptr_array_free(mono, FALSE);
}

/* Step cyclically through `v`. A value that isn't in the list (an uninstalled
 * font, a theme that vanished from disk) walks to the first entry rather than
 * pretending it was at some index. NULL when there is nowhere to go. */
static const char *step_list(char *const *v, const char *cur, int dir) {
  if (v == NULL || v[0] == NULL) return NULL;
  int n = 0;
  while (v[n] != NULL) n++;
  for (int i = 0; i < n; i++) {
    if (g_strcmp0(v[i], cur) == 0) return v[(i + dir + n) % n];
  }
  return v[0];
}

/* ---------- appearance ---------- */
static gboolean list_empty(char *const *v) { return v == NULL || v[0] == NULL; }

/* The themes the Theme row may walk: only the ones of the chosen appearance. */
static char *const *appearance_themes(PtSettings *s, gboolean dark) {
  return dark ? s->dark_themes : s->light_themes;
}

static gboolean classify_in_dir(const char *name, gpointer user) {
  return pt_theme_is_dark((const char *)user, name);
}

/* ---------- rows ---------- */
/* Highlight without rebuilding: the value labels are cached by pointer, so the
 * rows must outlive every keystroke. */
static void apply_selection(PtSettings *s) {
  int i = 0;
  for (GtkWidget *row = gtk_widget_get_first_child(s->list); row != NULL;
       row = gtk_widget_get_next_sibling(row), i++) {
    if (i == s->selected) gtk_widget_add_css_class(row, "selected");
    else gtk_widget_remove_css_class(row, "selected");
  }
}

static void add_row(PtSettings *s, int row, const char *name_text) {
  GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(r, "pt-settings-row");
  if (row == s->selected) gtk_widget_add_css_class(r, "selected");

  GtkWidget *name = gtk_label_new(name_text);
  gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
  gtk_widget_set_hexpand(name, TRUE);
  gtk_widget_set_valign(name, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(name, "pt-settings-name");
  gtk_box_append(GTK_BOX(r), name);

  GtkWidget *value_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_valign(value_box, GTK_ALIGN_CENTER);

  GtkWidget *left = gtk_label_new("‹");
  gtk_widget_add_css_class(left, "pt-settings-arrow");
  gtk_box_append(GTK_BOX(value_box), left);

  GtkWidget *value = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(value), 1.0f);
  gtk_label_set_ellipsize(GTK_LABEL(value), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(value), 28);
  gtk_widget_add_css_class(value, "pt-settings-value");
  gtk_box_append(GTK_BOX(value_box), value);
  s->value_labels[row] = value;

  GtkWidget *right = gtk_label_new("›");
  gtk_widget_add_css_class(right, "pt-settings-arrow");
  gtk_box_append(GTK_BOX(value_box), right);

  gtk_box_append(GTK_BOX(r), value_box);
  gtk_box_append(GTK_BOX(s->list), r);
}

static void rebuild_rows(PtSettings *s) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(s->list)) != NULL)
    gtk_box_remove(GTK_BOX(s->list), child);
  for (int i = 0; i < N_ROWS; i++) s->value_labels[i] = NULL;

  add_row(s, ROW_APPEARANCE, "Appearance");
  add_row(s, ROW_THEME, "Theme");
  add_row(s, ROW_FONT_SIZE, "Terminal font size");
  add_row(s, ROW_UI_FONT_SIZE, "UI font size");
  add_row(s, ROW_FONT_FAMILY, "Terminal font");
  add_row(s, ROW_UI_FONT_FAMILY, "UI font");
}

static void set_value(PtSettings *s, int row, char *text /* consumed */) {
  if (s->value_labels[row] != NULL)
    gtk_label_set_text(GTK_LABEL(s->value_labels[row]), text);
  g_free(text);
}

static void refresh_values(PtSettings *s) {
  if (s->candidate == NULL) return;
  set_value(s, ROW_APPEARANCE, g_strdup(s->appearance_dark ? "Dark" : "Light"));
  set_value(s, ROW_THEME, g_strdup(s->candidate->theme));
  set_value(s, ROW_FONT_SIZE, g_strdup_printf("%d pt", s->candidate->font_size));
  set_value(s, ROW_UI_FONT_SIZE, g_strdup_printf("%g px", s->candidate->ui_font_size));
  set_value(s, ROW_FONT_FAMILY, g_strdup(s->candidate->font_family));
  set_value(s, ROW_UI_FONT_FAMILY, g_strdup(s->candidate->ui_font_family));
}

/* ---------- editing ---------- */
/* Swap in a copy of `next` when it differs. `next` always points into one of
 * our own vectors, never into the field being freed. */
static gboolean replace_str(char **field, const char *next) {
  if (next == NULL || g_strcmp0(*field, next) == 0) return FALSE;
  g_free(*field);
  *field = g_strdup(next);
  return TRUE;
}

static void adjust(PtSettings *s, int dir) {
  if (!s->open || s->candidate == NULL) return;
  PtConfig *c = s->candidate;
  gboolean moved = FALSE;

  switch (s->selected) {
    case ROW_APPEARANCE: {
      /* Two values, so either arrow flips it. Never onto an appearance nothing
       * is installed for, though: that would leave the row below showing a
       * theme it cannot step off, out of a list it is not in. */
      char *const *next = appearance_themes(s, !s->appearance_dark);
      if (list_empty(next)) return;
      s->appearance_dark = !s->appearance_dark;
      /* The theme that was selected is of the other appearance, so it is not in
       * the narrowed list — land on its first entry and preview that. */
      replace_str(&c->theme, next[0]);
      moved = TRUE;
      break;
    }
    case ROW_THEME:
      moved = replace_str(
          &c->theme,
          step_list(appearance_themes(s, s->appearance_dark), c->theme, dir));
      break;
    case ROW_FONT_SIZE: {
      int next = CLAMP(c->font_size + dir, FONT_SIZE_MIN, FONT_SIZE_MAX);
      moved = next != c->font_size;
      c->font_size = next;
      break;
    }
    case ROW_UI_FONT_SIZE: {
      double next = CLAMP(c->ui_font_size + UI_FONT_SIZE_STEP * dir,
                          UI_FONT_SIZE_MIN, UI_FONT_SIZE_MAX);
      moved = fabs(next - c->ui_font_size) > 1e-9;
      c->ui_font_size = next;
      break;
    }
    case ROW_FONT_FAMILY:
      moved = replace_str(&c->font_family,
                          step_list(s->mono_fams, c->font_family, dir));
      break;
    case ROW_UI_FONT_FAMILY:
      moved = replace_str(&c->ui_font_family,
                          step_list(s->all_fams, c->ui_font_family, dir));
      break;
    default:
      return;
  }

  /* At a clamp end, or with a one-entry list, nothing actually moved — and
   * "changed" means a value moved, so the window gets no pointless re-apply. */
  if (!moved) return;
  refresh_values(s);
  g_signal_emit(s, signals[SIG_CHANGED], 0);
}

static void move_selection(PtSettings *s, int delta) {
  s->selected = CLAMP(s->selected + delta, 0, N_ROWS - 1);
  apply_selection(s);
}

static void commit_and_close(PtSettings *s) {
  if (!s->open) return;
  g_signal_emit(s, signals[SIG_COMMITTED], 0);
  pt_settings_close(s);
}

static void revert_and_close(PtSettings *s) {
  if (!s->open) return;
  g_signal_emit(s, signals[SIG_REVERTED], 0);
  pt_settings_close(s);
}

/* ---------- input ---------- */
static gboolean on_key(GtkEventControllerKey *ctl, guint keyval, guint keycode,
                       GdkModifierType state, gpointer user) {
  (void)ctl; (void)keycode;
  PtSettings *s = user;
  if (!s->open) return FALSE;

  switch (keyval) {
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
      move_selection(s, 1);
      return TRUE;
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
      move_selection(s, -1);
      return TRUE;
    /* Tab must never fall through: nothing inside the dialog is focusable
     * besides the widget itself, so GTK would hand focus to the terminal
     * underneath and every later keystroke would land there instead. */
    case GDK_KEY_Tab:
    case GDK_KEY_KP_Tab:
      move_selection(s, (state & GDK_SHIFT_MASK) != 0 ? -1 : 1);
      return TRUE;
    case GDK_KEY_ISO_Left_Tab:
      move_selection(s, -1);
      return TRUE;
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
      adjust(s, -1);
      return TRUE;
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
      adjust(s, 1);
      return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter:
      commit_and_close(s);
      return TRUE;
    case GDK_KEY_Escape:
      revert_and_close(s);
      return TRUE;
    default:
      return FALSE;
  }
}

/* Anything outside the panel dismisses, and dismissing is a cancel. */
static void on_scrim_pressed(GtkGestureClick *g, int n, double x, double y,
                             gpointer user) {
  (void)g; (void)n;
  PtSettings *s = user;
  if (!s->open) return;
  GtkWidget *hit = gtk_widget_pick(s->scrim, x, y, GTK_PICK_DEFAULT);
  for (GtkWidget *a = hit; a != NULL; a = gtk_widget_get_parent(a))
    if (a == s->panel) return;
  revert_and_close(s);
}

/* ---------- public API ---------- */
void pt_settings_open(PtSettings *s, const PtConfig *current,
                      const char *const *themes) {
  g_return_if_fail(PT_IS_SETTINGS(s));
  g_return_if_fail(current != NULL);

  g_clear_pointer(&s->candidate, pt_config_free);
  g_clear_pointer(&s->dark_themes, g_strfreev);
  g_clear_pointer(&s->light_themes, g_strfreev);
  s->candidate = pt_config_copy(current);
  /* Fonts can be installed while pt runs; a snapshot per open is cheap. */
  collect_families(s);

  /* Classify every theme once, here: it reads and parses each theme file, which
   * is fine per open (they are tiny and there are a handful) and would not be
   * per keypress. One partitioning walk, so no name is read twice. Same helper
   * the terminal answers CSI ? 996 n from, so the picker and what apps are told
   * can never disagree. */
  char *tdir = pt_theme_dir();
  pt_theme_filter_appearance(themes, classify_in_dir, tdir,
                             &s->dark_themes, &s->light_themes);
  s->appearance_dark = pt_theme_is_dark(tdir, s->candidate->theme);
  g_free(tdir);
  /* A theme that is no longer on disk classifies dark without being in the dark
   * list; if that leaves the row below empty while the other side has themes,
   * open on the side that has them rather than on nothing. */
  if (list_empty(appearance_themes(s, s->appearance_dark)) &&
      !list_empty(appearance_themes(s, !s->appearance_dark)))
    s->appearance_dark = !s->appearance_dark;

  s->selected = 0;
  s->open = TRUE;
  gtk_widget_set_visible(GTK_WIDGET(s), TRUE);
  gtk_widget_set_can_target(GTK_WIDGET(s), TRUE);
  refresh_values(s);
  apply_selection(s);
  /* The widget itself takes focus: the key controller is on it, and there is
   * nothing else in here that wants keys. */
  gtk_widget_grab_focus(GTK_WIDGET(s));
}

void pt_settings_close(PtSettings *s) {
  g_return_if_fail(PT_IS_SETTINGS(s));
  if (!s->open) return;
  s->open = FALSE;
  gtk_widget_set_visible(GTK_WIDGET(s), FALSE);
  /* Belt and braces: an invisible widget is not picked, but this also keeps the
   * overlay from swallowing clicks meant for the terminal underneath. */
  gtk_widget_set_can_target(GTK_WIDGET(s), FALSE);
  /* The candidate survives until the next open: a "closed" handler still wants
   * to read what was on screen. */
  g_signal_emit(s, signals[SIG_CLOSED], 0);
}

gboolean pt_settings_is_open(PtSettings *s) {
  g_return_val_if_fail(PT_IS_SETTINGS(s), FALSE);
  return s->open;
}

const PtConfig *pt_settings_config(PtSettings *s) {
  g_return_val_if_fail(PT_IS_SETTINGS(s), NULL);
  return s->candidate;
}

/* ---------- GObject ---------- */
/* No "closed" here on purpose: the window is on its way out too, and its
 * handler would reach for state that dispose has already dropped. */
static void pt_settings_dispose(GObject *obj) {
  PtSettings *s = PT_SETTINGS(obj);
  s->open = FALSE;
  g_clear_pointer(&s->candidate, pt_config_free);
  g_clear_pointer(&s->dark_themes, g_strfreev);
  g_clear_pointer(&s->light_themes, g_strfreev);
  g_clear_pointer(&s->mono_fams, g_strfreev);
  g_clear_pointer(&s->all_fams, g_strfreev);
  g_clear_pointer(&s->scrim, gtk_widget_unparent);
  s->panel = s->list = NULL;
  for (int i = 0; i < N_ROWS; i++) s->value_labels[i] = NULL;
  G_OBJECT_CLASS(pt_settings_parent_class)->dispose(obj);
}

static void pt_settings_class_init(PtSettingsClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_settings_dispose;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass),
                                           GTK_TYPE_BIN_LAYOUT);
  signals[SIG_CHANGED] = g_signal_new("changed", PT_TYPE_SETTINGS,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_COMMITTED] = g_signal_new("committed", PT_TYPE_SETTINGS,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_REVERTED] = g_signal_new("reverted", PT_TYPE_SETTINGS,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_CLOSED] = g_signal_new("closed", PT_TYPE_SETTINGS,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void pt_settings_init(PtSettings *s) {
  gtk_widget_set_visible(GTK_WIDGET(s), FALSE);
  gtk_widget_set_can_target(GTK_WIDGET(s), FALSE);
  /* Focusable so the CAPTURE key controller below actually sees keys. */
  gtk_widget_set_focusable(GTK_WIDGET(s), TRUE);

  s->scrim = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(s->scrim, "pt-palette-scrim");
  gtk_widget_set_hexpand(s->scrim, TRUE);
  gtk_widget_set_vexpand(s->scrim, TRUE);
  gtk_widget_set_parent(s->scrim, GTK_WIDGET(s));

  s->panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(s->panel, "pt-settings");
  gtk_widget_set_halign(s->panel, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(s->panel, GTK_ALIGN_START);
  gtk_widget_set_margin_top(s->panel, 90);
  gtk_widget_set_size_request(s->panel, PT_SETTINGS_WIDTH, -1);
  gtk_box_append(GTK_BOX(s->scrim), s->panel);

  GtkWidget *title = gtk_label_new("Settings");
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_widget_add_css_class(title, "pt-settings-title");
  gtk_box_append(GTK_BOX(s->panel), title);

  s->list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(s->panel), s->list);
  /* The six rows never change shape, only their values and highlight. */
  rebuild_rows(s);

  GtkWidget *hint = gtk_label_new(
      "↑↓ select · ←→ adjust · Enter save · Esc cancel");
  gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
  gtk_widget_add_css_class(hint, "pt-settings-hint");
  gtk_box_append(GTK_BOX(s->panel), hint);

  /* CAPTURE, like the palette: intercept on the way down so no child (and no
   * default focus handling) gets to the arrows, Enter, Escape or Tab first. */
  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), s);
  gtk_widget_add_controller(GTK_WIDGET(s), keys);

  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click, "pressed", G_CALLBACK(on_scrim_pressed), s);
  gtk_widget_add_controller(s->scrim, GTK_EVENT_CONTROLLER(click));
}

GtkWidget *pt_settings_new(void) {
  return g_object_new(PT_TYPE_SETTINGS, NULL);
}
