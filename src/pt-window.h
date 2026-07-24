/* pt-window.h */
#pragma once
#include <adwaita.h>
#define PT_TYPE_WINDOW (pt_window_get_type())
G_DECLARE_FINAL_TYPE(PtWindow, pt_window, PT, WINDOW, AdwApplicationWindow)
GtkWidget *pt_window_new(AdwApplication *app);
