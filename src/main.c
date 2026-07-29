#include <adwaita.h>
#include "pt-window.h"

static void on_activate(AdwApplication *app, gpointer user_data) {
  (void)user_data;
  GtkCssProvider *css = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(css, "/dev/hdprajwal/pt/style.css");
  /* Above USER, not APPLICATION: ~/.config/gtk-4.0/gtk.css loads at USER
   * priority and priority beats specificity in GTK's cascade, so desktop
   * themes with a `* { padding: 0 }` reset (e.g. Breeze) would strip every
   * padding in the app's own stylesheet. pt owns its design entirely. */
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
      GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
  g_object_unref(css);
  /* Bundled symbolic icons (pt-zed-symbolic). The theme scans for the
   * scalable/actions layout under this prefix, so hand it the icons root, not
   * the file. */
  gtk_icon_theme_add_resource_path(
      gtk_icon_theme_get_for_display(gdk_display_get_default()),
      "/dev/hdprajwal/pt/icons");
  /* The window owns the config and installs the theme provider itself. */
  gtk_window_present(GTK_WINDOW(pt_window_new(app)));
}

int main(int argc, char *argv[]) {
  AdwApplication *app =
      adw_application_new("dev.hdprajwal.pt", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  /* Before the run loop, not from the window: a click on a notification can
   * reach this process as a bare action activation, with no "activate" signal
   * and so no window, and an action that is not there yet is dropped. */
  pt_window_install_app_actions(app);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
