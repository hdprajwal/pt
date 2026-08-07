#include "pt-usage.h"
#include <string.h>

void pt_usage_clear(PtUsage *u) {
  if (u != NULL) memset(u, 0, sizeof(*u));
}

gboolean pt_usage_has_data(const PtUsage *u) {
  return u != NULL && u->fetched_at > 0;
}

/* g_strlcpy truncates at a byte, and every label here can carry a "·" that a
 * caller assembled from a window name it did not choose. Cutting one of those
 * in half puts invalid UTF-8 into a GtkLabel, which Pango warns about and
 * renders as garbage. This is the single point where a label enters the model,
 * so the boundary is enforced once, here. */
static void copy_label(char *dst, gsize size, const char *src) {
  if (src == NULL) { dst[0] = '\0'; return; }
  gsize len = strlen(src);
  if (len >= size) {
    const char *cut = src + size - 1;
    const char *prev = g_utf8_find_prev_char(src, cut);
    /* NULL means `cut` was already a character start, or src holds no valid
     * start at all — a whole-character boundary either way. */
    len = prev != NULL && g_utf8_next_char(prev) > cut ? (gsize)(prev - src)
                                                       : (gsize)(cut - src);
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

void pt_usage_add_window(PtUsage *u, const char *label, double percent,
                         gint64 resets_at) {
  if (u == NULL || u->n_windows >= PT_USAGE_MAX_WINDOWS) return;
  PtUsageWindow *w = &u->windows[u->n_windows++];
  copy_label(w->label, sizeof(w->label), label);
  /* Clamped rather than rejected: a provider that reports 103% has still told
   * us the window is full, and a bar drawn past its track is a rendering bug
   * on top of a reporting one. */
  w->percent = CLAMP(percent, 0.0, 100.0);
  w->resets_at = resets_at > 0 ? resets_at : 0;
}

int pt_usage_context_percent(const PtUsage *u) {
  if (u == NULL || u->ctx_limit <= 0 || u->ctx_used <= 0) return -1;
  gint64 pct = u->ctx_used * 100 / u->ctx_limit;
  return (int)CLAMP(pct, 0, 100);
}

char *pt_usage_format_duration(gint64 seconds) {
  if (seconds <= 0) return g_strdup("now");
  if (seconds < 60) return g_strdup("<1m");
  gint64 minutes = seconds / 60;
  if (minutes < 60) return g_strdup_printf("%" G_GINT64_FORMAT "m", minutes);
  gint64 hours = minutes / 60;
  minutes %= 60;
  if (hours < 24) {
    if (minutes == 0) return g_strdup_printf("%" G_GINT64_FORMAT "h", hours);
    return g_strdup_printf("%" G_GINT64_FORMAT "h %" G_GINT64_FORMAT "m",
                           hours, minutes);
  }
  gint64 days = hours / 24;
  hours %= 24;
  if (hours == 0) return g_strdup_printf("%" G_GINT64_FORMAT "d", days);
  return g_strdup_printf("%" G_GINT64_FORMAT "d %" G_GINT64_FORMAT "h",
                         days, hours);
}

char *pt_usage_format_age(gint64 fetched_at, gint64 now) {
  if (fetched_at <= 0) return NULL;
  gint64 age = now - fetched_at;
  /* A clock that moved backwards (a resume, an ntp step) would otherwise
   * print a negative age; the reading is current, so say so. */
  /* Under a minute is "just now", not "0m ago": a minute is the resolution
   * everything below works in, and rounding into it would print a zero. */
  if (age < 60) return g_strdup("just now");
  if (age < 3600) return g_strdup_printf("%" G_GINT64_FORMAT "m ago", age / 60);
  if (age < 86400)
    return g_strdup_printf("%" G_GINT64_FORMAT "h ago", age / 3600);
  return g_strdup_printf("%" G_GINT64_FORMAT "d ago", age / 86400);
}
