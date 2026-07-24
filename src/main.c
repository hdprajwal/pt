#include <adwaita.h>
#include "pt-terminal.h"

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
  GtkWidget *term = pt_terminal_new(NULL);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), term);
  gtk_window_present(GTK_WINDOW(win));
  gtk_widget_grab_focus(term);
}

int main(int argc, char *argv[]) {
  AdwApplication *app =
      adw_application_new("dev.hdprajwal.pt", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
