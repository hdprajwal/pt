#include "pt-agent-monitor.h"
#include "pt-claude-usage.h"
#include "pt-codex-usage.h"
#include <gio/gio.h>
#include <string.h>

/* How long a /proc descendant walk's answer is trusted. The foreground-command
 * fast path costs nothing and covers the usual case, so this only bounds how
 * often the walk itself runs for a pane whose agent is buried under a wrapper.
 * Half a second of lag on the section appearing is not worth a directory walk
 * twice a second. */
#define PT_AGENT_DETECT_TTL_US (2 * G_USEC_PER_SEC)
/* The floor under a lookup that something other than the timer asked for.
 *
 * A new agent, or a new directory, is a different question and answering it at
 * the next tick would leave the previous pane's session on screen for up to
 * two minutes. But the directory comes from whichever pane has focus, so
 * ⌃⇥ held down is a directory change per keystroke — and without a floor each
 * one would be a request to Anthropic or a pass over the session logs. Fast
 * enough to feel immediate, slow enough that browsing tabs cannot become a
 * request stream. */
#define PT_AGENT_EVENT_MIN_S 5
/* Backoff when a 429 arrives without a Retry-After to obey. */
#define PT_AGENT_DEFAULT_BACKOFF_S 300
/* Nothing about a usage lookup should be able to wedge the panel. */
#define PT_AGENT_CURL_TIMEOUT_S "10"

/* One slot per agent, indexed by PtAgentKind — including the unused NONE slot,
 * so the kind can index this directly and no mapping can go wrong. */
#define PT_AGENT_SLOTS (PT_AGENT_CODEX + 1)

typedef struct {
  PtUsage usage;      /* the last good reading; fetched_at 0 = never */
  char *error;        /* the last failure, kept alongside the reading */
  gint64 last_try;    /* unix seconds; when a lookup was last started */
  gint64 next_poll;   /* unix seconds; a lookup before this is not due */
  gint64 blocked_til; /* unix seconds; a rate limit nobody may bypass */
} Slot;

/* Who is asking, which is what decides how recently a lookup may already have
 * happened. The rate limit outranks all three. */
typedef enum {
  FETCH_DUE,    /* the timer: the full poll interval must have passed */
  FETCH_EVENT,  /* a changed question: only the event floor */
  FETCH_USER,   /* the refresh button: now, because someone asked for it */
} PtFetchWhy;

struct PtAgentMonitor {
  PtAgentMonitorCb cb;
  gpointer user;

  gboolean visible;
  PtAgentKind kind;
  char *cwd;
  int shell_pid;

  /* Detection state: the standing answer, and when it was last confirmed by
   * either route. The fast path refreshes the timestamp too, so what the TTL
   * actually measures is how long since the agent was last seen at all —
   * which is what makes the answer survive the tick or two where a tab switch
   * reads the process list empty. */
  PtAgentKind walked_kind;
  int walked_pid;
  gint64 walked_at_us;

  Slot slots[PT_AGENT_SLOTS];

  gboolean claude_enabled;
  gboolean claude_inflight;
  guint generation;   /* discards a reply for a question no longer asked */
  guint timer;
  gboolean freed;     /* a subprocess is still in flight over the free */
  int refs;
};

static void monitor_unref(PtAgentMonitor *m) {
  if (--m->refs > 0) return;
  for (int i = 0; i < PT_AGENT_SLOTS; i++) g_free(m->slots[i].error);
  g_free(m->cwd);
  g_free(m);
}

static Slot *slot_for(PtAgentMonitor *m, PtAgentKind kind) {
  if (kind <= PT_AGENT_NONE || kind >= PT_AGENT_SLOTS) return NULL;
  return &m->slots[kind];
}

static void notify(PtAgentMonitor *m) {
  if (!m->freed && m->cb != NULL) m->cb(m->user);
}

/* A failure never clears the reading it lands on top of. */
static void slot_fail(PtAgentMonitor *m, Slot *s, const char *fmt, ...)
    G_GNUC_PRINTF(3, 4);

static void slot_fail(PtAgentMonitor *m, Slot *s, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *msg = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  g_free(s->error);
  s->error = msg;
  notify(m);
}

static void slot_ok(PtAgentMonitor *m, Slot *s, const PtUsage *u, gint64 now) {
  s->usage = *u;
  s->usage.fetched_at = now;
  g_clear_pointer(&s->error, g_free);
  notify(m);
}

/* ---------- codex ---------- */
static void fetch_codex(PtAgentMonitor *m, Slot *s, gint64 now) {
  char *home = pt_codex_home();
  PtUsage u;
  gboolean ok = pt_codex_usage_read(home, m->cwd, now, &u);
  g_free(home);
  if (ok) slot_ok(m, s, &u, now);
  else    slot_fail(m, s, "no recent Codex session log");
}

/* ---------- claude ---------- */
typedef struct { PtAgentMonitor *m; guint generation; } ClaudeCall;

static void on_claude_done(GObject *src, GAsyncResult *res, gpointer user) {
  ClaudeCall *call = user;
  PtAgentMonitor *m = call->m;
  char *out = NULL;
  GSubprocess *proc = G_SUBPROCESS(src);
  GError *err = NULL;
  gboolean ran = g_subprocess_communicate_utf8_finish(proc, res, &out, NULL,
                                                      &err);
  g_object_unref(proc);
  m->claude_inflight = FALSE;

  /* A reply to a question nobody is asking any more: the pane moved on, or
   * the window is gone. Either way it must not touch the slots. */
  if (m->freed || call->generation != m->generation) goto out;

  Slot *s = slot_for(m, PT_AGENT_CLAUDE);
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  if (!ran || out == NULL) {
    slot_fail(m, s, "could not reach the usage endpoint");
    goto out;
  }

  int status = 0;
  gint64 retry_after = 0;
  const char *body = pt_claude_http_split(out, &status, &retry_after);
  if (status == 429) {
    /* Obeyed, never guessed at. A 429 that gets hammered lasts longer, and
     * the refresh button is gated on this too. */
    gint64 wait = retry_after > 0 ? retry_after : PT_AGENT_DEFAULT_BACKOFF_S;
    s->blocked_til = now + wait;
    s->next_poll = s->blocked_til;
    char *in = pt_usage_format_duration(wait);
    slot_fail(m, s, "rate limited — retrying in %s", in);
    g_free(in);
  } else if (status == 401 || status == 403) {
    slot_fail(m, s, "Anthropic rejected the login — run claude again");
  } else if (status < 200 || status >= 300) {
    slot_fail(m, s, "usage lookup failed (HTTP %d)", status);
  } else {
    PtUsage u;
    if (pt_claude_usage_parse(body, now, &u)) {
      /* The response does not always name the plan; the stored login does, and
       * a plan already learned does not un-learn itself on one reply. */
      if (u.plan[0] == '\0' && s->usage.plan[0] != '\0')
        g_strlcpy(u.plan, s->usage.plan, sizeof(u.plan));
      slot_ok(m, s, &u, now);
    } else {
      slot_fail(m, s, "usage response not understood");
    }
  }

out:
  g_clear_error(&err);
  g_free(out);
  monitor_unref(m);
  g_free(call);
}

static void fetch_claude(PtAgentMonitor *m, Slot *s, gint64 now) {
  if (m->claude_inflight) return;
  char *path = pt_claude_creds_path();
  PtClaudeCreds creds;
  gboolean have = pt_claude_creds_read(path, &creds);
  g_free(path);
  if (!have) {
    slot_fail(m, s, "no Claude login found — run claude");
    pt_claude_creds_clear(&creds);
    return;
  }
  /* Expired is reported, never repaired. Refreshing the token would rotate the
   * one Claude Code itself depends on and log the user out of their own CLI —
   * a steep price for a progress bar. */
  if (creds.expires_at > 0 && creds.expires_at <= now) {
    slot_fail(m, s, "Claude login expired — run claude");
    pt_claude_creds_clear(&creds);
    return;
  }
  /* A token is base64url text. Anything that could close the quoted string in
   * curl's config is not one, and is not going to be quoted into it. */
  if (strpbrk(creds.token, "\"\\\r\n") != NULL) {
    slot_fail(m, s, "stored login is not in a form pt can use");
    pt_claude_creds_clear(&creds);
    return;
  }
  /* Remember the plan from the login itself: the response does not always
   * name it, and this is a local read either way. */
  if (creds.plan[0] != '\0' && s->usage.plan[0] == '\0')
    g_strlcpy(s->usage.plan, creds.plan, sizeof(s->usage.plan));

  GError *err = NULL;
  GSubprocess *proc = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
          G_SUBPROCESS_FLAGS_STDERR_SILENCE,
      &err, "curl", "-s", "-i", "--max-time", PT_AGENT_CURL_TIMEOUT_S,
      "--config", "-", PT_CLAUDE_USAGE_URL, NULL);
  if (proc == NULL) {
    g_clear_error(&err);
    slot_fail(m, s, "curl is not installed");
    pt_claude_creds_clear(&creds);
    return;
  }
  /* The token goes in over stdin, not in argv: everything on a process's
   * command line is world-readable out of /proc, and this is the whole of the
   * user's Claude login. */
  char *config = g_strdup_printf(
      "header = \"Authorization: Bearer %s\"\n"
      "header = \"anthropic-beta: " PT_CLAUDE_OAUTH_BETA "\"\n"
      "header = \"Accept: application/json\"\n",
      creds.token);
  pt_claude_creds_clear(&creds);

  ClaudeCall *call = g_new0(ClaudeCall, 1);
  call->m = m;
  call->generation = m->generation;
  m->refs++;
  m->claude_inflight = TRUE;
  g_subprocess_communicate_utf8_async(proc, config, NULL, on_claude_done, call);
  /* GLib copied this into its own buffer, so wiping ours only clears pt's
   * copy — but pt's copy is the one pt is responsible for. */
  memset(config, 0, strlen(config));
  g_free(config);
  notify(m);   /* the card says it is checking */
}

/* ---------- scheduling ---------- */
static void maybe_fetch(PtAgentMonitor *m, PtFetchWhy why) {
  Slot *s = slot_for(m, m->kind);
  if (s == NULL || !m->visible) return;
  /* Off is off. The gate lives here rather than at the button so that no path
   * into a lookup — a timer tick, a directory change, the refresh button —
   * can reach the network without passing it. */
  if (m->kind == PT_AGENT_CLAUDE && !m->claude_enabled) return;
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  /* The one gate nothing steps around, the refresh button included:
   * hammering a 429 only makes it last longer. */
  if (s->blocked_til > now) return;
  if (why == FETCH_DUE && s->next_poll > now) return;
  if (why == FETCH_EVENT && s->last_try + PT_AGENT_EVENT_MIN_S > now) return;
  s->last_try = now;
  s->next_poll = now + PT_AGENT_POLL_S;
  switch (m->kind) {
    case PT_AGENT_CODEX:  fetch_codex(m, s, now);  break;
    case PT_AGENT_CLAUDE: fetch_claude(m, s, now); break;
    case PT_AGENT_NONE:   break;
  }
}

static gboolean on_timer(gpointer user) {
  PtAgentMonitor *m = user;
  maybe_fetch(m, FETCH_DUE);
  return G_SOURCE_CONTINUE;
}

/* The timer only runs while there is something to poll for. With the panel
 * closed, or with no agent in the pane, nothing is polled at all. */
static void sync_timer(PtAgentMonitor *m) {
  gboolean want = m->visible && m->kind != PT_AGENT_NONE;
  if (want && m->timer == 0) {
    m->timer = g_timeout_add_seconds(PT_AGENT_POLL_S, on_timer, m);
  } else if (!want && m->timer != 0) {
    g_source_remove(m->timer);
    m->timer = 0;
  }
}

/* ---------- public ---------- */
PtAgentMonitor *pt_agent_monitor_new(PtAgentMonitorCb cb, gpointer user) {
  PtAgentMonitor *m = g_new0(PtAgentMonitor, 1);
  m->cb = cb;
  m->user = user;
  m->refs = 1;
  return m;
}

void pt_agent_monitor_free(PtAgentMonitor *m) {
  if (m == NULL) return;
  m->freed = TRUE;
  m->cb = NULL;
  if (m->timer != 0) g_source_remove(m->timer);
  m->timer = 0;
  monitor_unref(m);
}

void pt_agent_monitor_observe(PtAgentMonitor *m, gboolean panel_visible,
                              int shell_pid, const char *fg_name,
                              const char *cwd) {
  if (m == NULL) return;
  gboolean was_visible = m->visible;
  m->visible = panel_visible;
  /* Closed panel: stop the timer and stop looking. The detected kind is left
   * alone rather than cleared — clearing it would make the next open look
   * like a change of agent, and a forced lookup on every toggle of the panel
   * is exactly the hammering the poll interval exists to prevent. */
  if (!panel_visible) {
    sync_timer(m);
    return;
  }

  /* Detection, cheap path first. A match on the pane's foreground command is
   * free — pt polls that anyway — and answers for a plainly-run agent. Only a
   * miss pays for the descendant walk, and only every so often. */
  PtAgentKind kind = pt_agent_kind_from_name(fg_name);
  if (kind == PT_AGENT_NONE) {
    gint64 now_us = g_get_monotonic_time();
    if (shell_pid != m->shell_pid ||
        now_us - m->walked_at_us >= PT_AGENT_DETECT_TTL_US) {
      m->walked_kind = pt_agent_detect(shell_pid, NULL, &m->walked_pid);
      m->walked_at_us = now_us;
      m->shell_pid = shell_pid;
    }
    kind = m->walked_kind;
  } else {
    /* The fast path answered, so the walk's cached answer is stale in the
     * only direction that matters: it must not outlive this agent. */
    m->walked_kind = kind;
    m->walked_at_us = g_get_monotonic_time();
    m->shell_pid = shell_pid;
  }

  gboolean kind_changed = kind != m->kind;
  gboolean cwd_changed = g_strcmp0(cwd, m->cwd) != 0;
  if (cwd_changed) {
    g_free(m->cwd);
    m->cwd = g_strdup(cwd);
  }
  if (kind_changed) {
    /* A different agent is a different question, so a reply still in flight is
     * answering the old one and must not land on the new. A directory change
     * is not: Codex reads a different log, but nothing is in flight for it,
     * and Claude's answer never depended on the directory at all. */
    m->generation++;
    m->kind = kind;
  }
  sync_timer(m);

  if (kind == PT_AGENT_NONE) {
    if (kind_changed) notify(m);
    return;
  }
  /* Which changes are worth asking again over.
   *
   * The directory only matters to Codex, and only for the context bar — the
   * limits are account-wide. Claude's whole answer is account-wide, so a pane
   * switch there changes nothing that could be looked up, and treating it as a
   * new question would put a request on the wire per ⌃⇥.
   *
   * The first look at an agent that has never reported is the other one: it is
   * what fills the card the moment the panel opens. Reopening the panel on a
   * reading a minute old asks nothing — the reading is still true. */
  Slot *s = slot_for(m, kind);
  gboolean first_look = !pt_usage_has_data(&s->usage) && s->error == NULL;
  gboolean new_question =
      kind_changed || first_look || (cwd_changed && kind == PT_AGENT_CODEX);
  if (new_question)      maybe_fetch(m, FETCH_EVENT);
  else if (!was_visible) maybe_fetch(m, FETCH_DUE);
  if (kind_changed || cwd_changed) notify(m);
}

void pt_agent_monitor_set_claude_enabled(PtAgentMonitor *m, gboolean on) {
  if (m == NULL || m->claude_enabled == on) return;
  m->claude_enabled = on;
  Slot *s = slot_for(m, PT_AGENT_CLAUDE);
  /* Either direction invalidates a reply in flight: turning it off must not
   * let one land after the user withdrew permission, and turning it on starts
   * a fresh one. */
  m->generation++;
  if (!on) {
    /* Turning it off throws the numbers away as well as stopping the lookups.
     * Leaving a plan's limits on screen after the user withdrew permission to
     * fetch them would be keeping something they asked pt to stop having. */
    pt_usage_clear(&s->usage);
    g_clear_pointer(&s->error, g_free);
    s->last_try = 0;
    s->next_poll = 0;
  } else if (m->kind == PT_AGENT_CLAUDE) {
    /* The press of Turn on is the user asking, so it goes now. */
    maybe_fetch(m, FETCH_USER);
  }
  notify(m);
}

void pt_agent_monitor_refresh(PtAgentMonitor *m) {
  if (m == NULL) return;
  maybe_fetch(m, FETCH_USER);
}

void pt_agent_monitor_view(PtAgentMonitor *m, PtAgentView *out) {
  memset(out, 0, sizeof(*out));
  if (m == NULL) return;
  out->kind = m->kind;
  Slot *s = slot_for(m, m->kind);
  if (s == NULL) return;
  out->needs_optin = m->kind == PT_AGENT_CLAUDE && !m->claude_enabled;
  out->busy = m->kind == PT_AGENT_CLAUDE && m->claude_inflight;
  if (out->needs_optin) return;
  /* The reading and the error are both handed over: a stale number under a
   * warning is more use than an empty card, which is the whole point of
   * keeping them side by side. */
  if (pt_usage_has_data(&s->usage)) out->usage = &s->usage;
  out->error = s->error;
}
