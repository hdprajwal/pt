#include <adwaita.h>
#include "pt-window.h"

static void on_activate(AdwApplication *app, gpointer user_data) {
  (void)user_data;
  GtkCssProvider *css = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(css, "/dev/hdprajwal/pt/style.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
      GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER);
  g_object_unref(css);
  gtk_window_present(GTK_WINDOW(pt_window_new(app)));
}

int main(int argc, char *argv[]) {
  AdwApplication *app =
      adw_application_new("dev.hdprajwal.pt", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
