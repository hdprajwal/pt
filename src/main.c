#include <adwaita.h>
#include "pt-pane-grid.h"
#include "pt-split-tree.h"

/* Temporary manual-test shortcuts; replaced by PtWindow in Task 10. */
static gboolean cb_split_h(GtkWidget *w, GVariant *args, gpointer user) {
  (void)w; (void)args;
  pt_pane_grid_split(PT_PANE_GRID(user), PT_SPLIT_H);
  return TRUE;
}
static gboolean cb_split_v(GtkWidget *w, GVariant *args, gpointer user) {
  (void)w; (void)args;
  pt_pane_grid_split(PT_PANE_GRID(user), PT_SPLIT_V);
  return TRUE;
}
static gboolean cb_close(GtkWidget *w, GVariant *args, gpointer user) {
  (void)w; (void)args;
  pt_pane_grid_close_focused(PT_PANE_GRID(user));
  return TRUE;
}
static gboolean cb_focus_next(GtkWidget *w, GVariant *args, gpointer user) {
  (void)w; (void)args;
  pt_pane_grid_focus_next(PT_PANE_GRID(user));
  return TRUE;
}

static void add_shortcut(GtkShortcutController *ctl, const char *trigger,
                         GtkShortcutFunc cb, gpointer grid) {
  gtk_shortcut_controller_add_shortcut(
      ctl, gtk_shortcut_new(gtk_shortcut_trigger_parse_string(trigger),
                            gtk_callback_action_new(cb, grid, NULL)));
}

static void on_activate(AdwApplication *app, gpointer user_data) {
  (void)user_data;
  GtkCssProvider *css = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(css, "/dev/hdprajwal/pt/style.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
      GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);

  GtkWidget *win = adw_application_window_new(GTK_APPLICATION(app));
  gtk_window_set_title(GTK_WINDOW(win), "pt");
  gtk_window_set_default_size(GTK_WINDOW(win), 1100, 700);

  GtkWidget *grid = pt_pane_grid_new(pt_split_leaf_new(NULL));
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), grid);

  GtkShortcutController *ctl = GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());
  gtk_shortcut_controller_set_scope(ctl, GTK_SHORTCUT_SCOPE_GLOBAL);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(ctl),
                                             GTK_PHASE_CAPTURE);
  add_shortcut(ctl, "<Control><Shift>d", cb_split_h, grid);
  add_shortcut(ctl, "<Control><Shift>s", cb_split_v, grid);
  add_shortcut(ctl, "<Control><Shift>w", cb_close, grid);
  add_shortcut(ctl, "<Control><Shift>o", cb_focus_next, grid);
  gtk_widget_add_controller(win, GTK_EVENT_CONTROLLER(ctl));

  gtk_window_present(GTK_WINDOW(win));
  pt_pane_grid_focus_terminal(PT_PANE_GRID(grid));
}

int main(int argc, char *argv[]) {
  AdwApplication *app =
      adw_application_new("dev.hdprajwal.pt", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
