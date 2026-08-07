/* pt-usage.h — what every agent's usage reader produces and the panel shows.
 *
 * The three agents have nothing in common in how they report usage — one
 * writes a session log, one answers an HTTP call — so this is the shape they
 * are each translated into, once, at the edge of their reader. Everything past
 * that point (the panel, the cache, the staleness line) only ever sees this. */
#pragma once
#include <glib.h>
#include "pt-agent.h"

/* Enough for the most windows any of these agents reports today: Claude's
 * 5-hour, weekly and per-model weekly caps make three. */
#define PT_USAGE_MAX_WINDOWS 4

typedef struct {
  char label[24];     /* "5h", "weekly", "weekly · opus" — as shown */
  double percent;     /* 0..100, clamped on the way in */
  gint64 resets_at;   /* unix seconds; 0 when the source did not say */
} PtUsageWindow;

typedef struct {
  PtAgentKind kind;
  char plan[24];      /* "pro", "max", "plus"; "" when the source did not say */
  PtUsageWindow windows[PT_USAGE_MAX_WINDOWS];
  int n_windows;
  /* The session's context fill. Both must be > 0 for the bar to mean
   * anything, so both being 0 is how "this agent records no context" is
   * spelled. */
  gint64 ctx_used, ctx_limit;
  gboolean limit_hit; /* the source says a limit was actually reached */
  char source[48];    /* the line under the bars: where these came from */
  gint64 fetched_at;  /* unix seconds; 0 means never filled in */
} PtUsage;

/* Zeroed and ready to fill. */
void pt_usage_clear(PtUsage *u);

/* Whether there is a reading here at all. A usage struct that never got one
 * is not an empty reading, it is the absence of one, and the panel shows
 * nothing rather than a row of zeroes. */
gboolean pt_usage_has_data(const PtUsage *u);

/* Appends a limit window, clamping the percentage and truncating the label.
 * Silently drops anything past PT_USAGE_MAX_WINDOWS — a provider that grows a
 * fifth window should show four bars, not corrupt memory. */
void pt_usage_add_window(PtUsage *u, const char *label, double percent,
                         gint64 resets_at);

/* 0..100, or -1 when there is no context reading. */
int pt_usage_context_percent(const PtUsage *u);

/* "3h 20m", "2d 4h", "45m", "now". `seconds` <= 0 reads as "now": a window
 * whose reset time has passed has reset, the reading is just stale.
 * Caller frees. */
char *pt_usage_format_duration(gint64 seconds);

/* How old a reading is, for the source line: "just now", "4m ago", "2h ago".
 * `fetched_at` of 0 gives NULL. Caller frees. */
char *pt_usage_format_age(gint64 fetched_at, gint64 now);
