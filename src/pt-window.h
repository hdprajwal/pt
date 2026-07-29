/* pt-window.h */
#pragma once
#include <adwaita.h>
#define PT_TYPE_WINDOW (pt_window_get_type())
G_DECLARE_FINAL_TYPE(PtWindow, pt_window, PT, WINDOW, AdwApplicationWindow)
GtkWidget *pt_window_new(AdwApplication *app);
/* The application-level actions a desktop notification can activate by name.
 * Installed before g_application_run and not from the window, because the
 * activation that reaches them need not have gone through "activate" — the
 * desktop can dispatch an action at an application that has no window yet.
 * Idempotent. */
void pt_window_install_app_actions(AdwApplication *app);
