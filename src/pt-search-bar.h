/* pt-search-bar.h */
#pragma once
#include <gtk/gtk.h>

#define PT_TYPE_SEARCH_BAR (pt_search_bar_get_type())
G_DECLARE_FINAL_TYPE(PtSearchBar, pt_search_bar, PT, SEARCH_BAR, GtkWidget)

GtkWidget *pt_search_bar_new(void);

/* Slide the bar in and put the keyboard in its entry, selecting whatever
 * text is already there so typing replaces it. Close slides it out; the
 * window moves focus back to the terminal itself (the bar cannot do that —
 * it does not know which pane had it). */
void pt_search_bar_open(PtSearchBar *sb);
void pt_search_bar_close(PtSearchBar *sb);
gboolean pt_search_bar_is_open(PtSearchBar *sb);

/* The entry's current text. Borrowed; "" when empty. */
const char *pt_search_bar_text(PtSearchBar *sb);

/* "N/M": current match one-based within count, as in every editor's search
 * bar. With no matches at all the label reads "no results"; with no search
 * yet it is blank. */
void pt_search_bar_set_count(PtSearchBar *sb, int current, int count);
