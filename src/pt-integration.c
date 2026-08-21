/* pt-integration.c — installing the agent-side hooks.
 *
 * Everything here edits a file that belongs to the user, not to pt, so the
 * rules are the same throughout: add, never replace; refuse anything we did
 * not parse cleanly; and say what happened. The Claude half rewrites
 * ~/.claude/settings.json because JSON round-trips losslessly. The codex half
 * only prints the line to paste, because pt cannot round-trip TOML without
 * throwing away the comments and ordering the user wrote. */
#include "pt-integration.h"
#include "pt-json-read.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define PT_INTEGRATION_ERROR (g_quark_from_static_string("pt-integration"))

/* The hook events pt installs, and the agent-report arguments each one runs.
 * SessionStart registers the resume; Stop and Notification carry the
 * lifecycle events pt raises desktop notifications from. One row per event,
 * so the merge, the installed check and the status output all walk the same
 * table and cannot drift apart. */
static const struct { const char *event; const char *args; } claude_hooks[] = {
  { "SessionStart",  "agent-report claude" },
  { "Stop",          "agent-report claude-event Stop" },
  { "Notification",  "agent-report claude-event Notification" },
};

/* The full command line a hook runs: the installing binary by absolute path,
 * so an agent started under a different PATH still finds the same pt. */
static char *claude_command(const char *pt_bin, const char *args) {
  return g_strconcat(pt_bin, " ", args, NULL);
}

/* Whether any entry under hooks.<event> already runs `command`. */
static gboolean event_has_command(JsonObject *root, const char *event,
                                  const char *command) {
  JsonObject *hooks = pt_json_obj(root, "hooks");
  if (hooks == NULL) return FALSE;
  JsonArray *entries = pt_json_array(hooks, event);
  if (entries == NULL) return FALSE;
  guint n = json_array_get_length(entries);
  for (guint i = 0; i < n; i++) {
    JsonNode *node = json_array_get_element(entries, i);
    if (!JSON_NODE_HOLDS_OBJECT(node)) continue;
    JsonArray *inner = pt_json_array(json_node_get_object(node), "hooks");
    if (inner == NULL) continue;
    guint m = json_array_get_length(inner);
    for (guint j = 0; j < m; j++) {
      JsonNode *h = json_array_get_element(inner, j);
      if (!JSON_NODE_HOLDS_OBJECT(h)) continue;
      if (g_strcmp0(pt_json_string(json_node_get_object(h), "command"),
                    command) == 0)
        return TRUE;
    }
  }
  return FALSE;
}

/* NULL with err set on anything that is not a JSON object. Empty input is not
 * an error: a missing settings.json is the common case, and "{}" is what it
 * would have said. */
static JsonParser *parse_settings(const char *settings_text, GError **err) {
  JsonParser *p = json_parser_new();
  if (settings_text == NULL || *settings_text == '\0') {
    json_parser_load_from_data(p, "{}", -1, NULL);
    return p;
  }
  GError *local = NULL;
  if (!json_parser_load_from_data(p, settings_text, -1, &local)) {
    g_propagate_prefixed_error(err, local, "settings.json is not valid JSON: ");
    g_object_unref(p);
    return NULL;
  }
  if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
    g_set_error_literal(err, PT_INTEGRATION_ERROR, 0,
                        "settings.json does not hold a JSON object");
    g_object_unref(p);
    return NULL;
  }
  return p;
}

gboolean pt_integration_claude_installed(const char *settings_text,
                                         const char *pt_bin) {
  if (settings_text == NULL || pt_bin == NULL) return FALSE;
  JsonParser *p = parse_settings(settings_text, NULL);
  if (p == NULL) return FALSE;
  JsonObject *root = json_node_get_object(json_parser_get_root(p));
  gboolean all = TRUE;
  for (gsize i = 0; i < G_N_ELEMENTS(claude_hooks) && all; i++) {
    char *cmd = claude_command(pt_bin, claude_hooks[i].args);
    all = event_has_command(root, claude_hooks[i].event, cmd);
    g_free(cmd);
  }
  g_object_unref(p);
  return all;
}

gboolean pt_integration_claude_event_installed(const char *settings_text,
                                               const char *pt_bin,
                                               const char *event) {
  if (settings_text == NULL || pt_bin == NULL || event == NULL) return FALSE;
  const char *args = NULL;
  for (gsize i = 0; i < G_N_ELEMENTS(claude_hooks); i++)
    if (g_strcmp0(event, claude_hooks[i].event) == 0) args = claude_hooks[i].args;
  if (args == NULL) return FALSE;
  JsonParser *p = parse_settings(settings_text, NULL);
  if (p == NULL) return FALSE;
  char *cmd = claude_command(pt_bin, args);
  gboolean found = event_has_command(
      json_node_get_object(json_parser_get_root(p)), event, cmd);
  g_free(cmd);
  g_object_unref(p);
  return found;
}

/* Fetch a member of the expected type, creating it when absent or JSON null.
 * NULL when the member is there but is something else: a settings.json where
 * "hooks" is a string is one pt does not understand, and overwriting it would
 * lose whatever the user meant by it. */
static JsonObject *object_member(JsonObject *parent, const char *name) {
  if (pt_json_is_set(parent, name)) return pt_json_obj(parent, name);
  JsonObject *o = json_object_new();
  json_object_set_object_member(parent, name, o);
  return o;
}

static JsonArray *array_member(JsonObject *parent, const char *name) {
  if (pt_json_is_set(parent, name)) return pt_json_array(parent, name);
  JsonArray *a = json_array_new();
  json_object_set_array_member(parent, name, a);
  return a;
}

char *pt_integration_claude_merged_settings(const char *settings_text,
                                            const char *pt_bin,
                                            GError **err) {
  g_return_val_if_fail(pt_bin != NULL && *pt_bin != '\0', NULL);

  JsonParser *p = parse_settings(settings_text, err);
  if (p == NULL) return NULL;
  JsonObject *root = json_node_get_object(json_parser_get_root(p));

  /* Which events already carry their command. */
  char *cmds[G_N_ELEMENTS(claude_hooks)];
  gboolean present[G_N_ELEMENTS(claude_hooks)];
  gboolean all = TRUE;
  for (gsize i = 0; i < G_N_ELEMENTS(claude_hooks); i++) {
    cmds[i] = claude_command(pt_bin, claude_hooks[i].args);
    present[i] = event_has_command(root, claude_hooks[i].event, cmds[i]);
    all = all && present[i];
  }

  /* Already there: hand back the user's own bytes rather than a reformatted
   * copy, so a second install truly changes nothing on disk. */
  if (all) {
    for (gsize i = 0; i < G_N_ELEMENTS(claude_hooks); i++) g_free(cmds[i]);
    g_object_unref(p);
    return g_strdup(settings_text != NULL ? settings_text : "{}\n");
  }

  /* Validate every shape before touching anything: a merge that appended two
   * events and then refused on the third would have to be unwound, and the
   * refusal is meant to leave the file exactly as it was. "hooks" itself must
   * be an object, and each event key we append to must be absent or an
   * array — anything else is a shape pt did not parse, and overwriting it
   * would lose whatever the user meant by it. */
  if (pt_json_is_set(root, "hooks") && pt_json_obj(root, "hooks") == NULL) {
    g_set_error_literal(err, PT_INTEGRATION_ERROR, 0,
                        "settings.json already uses \"hooks\" in a shape"
                        " pt does not understand");
    goto refuse;
  }
  JsonObject *existing = pt_json_obj(root, "hooks");
  for (gsize i = 0; existing != NULL && i < G_N_ELEMENTS(claude_hooks); i++) {
    if (pt_json_is_set(existing, claude_hooks[i].event) &&
        pt_json_array(existing, claude_hooks[i].event) == NULL) {
      g_set_error(err, PT_INTEGRATION_ERROR, 0,
                  "settings.json already uses hooks.%s in a shape pt does"
                  " not understand", claude_hooks[i].event);
      goto refuse;
    }
  }

  JsonObject *hooks = object_member(root, "hooks");
  for (gsize i = 0; hooks != NULL && i < G_N_ELEMENTS(claude_hooks); i++) {
    if (present[i]) continue;
    JsonArray *entries = array_member(hooks, claude_hooks[i].event);
    /* One entry, no matcher: none of these events match on a tool. */
    JsonObject *hook = json_object_new();
    json_object_set_string_member(hook, "type", "command");
    json_object_set_string_member(hook, "command", cmds[i]);
    JsonArray *inner = json_array_new();
    json_array_add_object_element(inner, hook);
    JsonObject *entry = json_object_new();
    json_object_set_array_member(entry, "hooks", inner);
    json_array_add_object_element(entries, entry);
  }
  for (gsize i = 0; i < G_N_ELEMENTS(claude_hooks); i++) g_free(cmds[i]);

  JsonGenerator *gen = json_generator_new();
  json_generator_set_pretty(gen, TRUE);
  json_generator_set_indent(gen, 2);
  json_generator_set_root(gen, json_parser_get_root(p));
  char *body = json_generator_to_data(gen, NULL);
  g_object_unref(gen);
  g_object_unref(p);

  /* Trailing newline: this is a file people open in an editor. */
  char *out = g_strconcat(body, "\n", NULL);
  g_free(body);
  return out;

refuse:
  for (gsize i = 0; i < G_N_ELEMENTS(claude_hooks); i++) g_free(cmds[i]);
  g_object_unref(p);
  return NULL;
}

/* ---- the CLI ---- */

/* This very binary, by absolute path, so a build tree, a /usr/local install
 * and a distro package each point the hooks at the pt that installed them —
 * and an agent started from a login shell with a different PATH still finds
 * it. Falls back to the bare name for PATH lookup on a system without /proc.
 * Caller frees. */
static char *pt_binary_path(void) {
  char *exe = g_file_read_link("/proc/self/exe", NULL);
  if (exe != NULL) return exe;
  return g_strdup("pt");
}

static char *claude_settings_path(void) {
  return g_build_filename(g_get_home_dir(), ".claude", "settings.json", NULL);
}

static char *codex_config_path(void) {
  return g_build_filename(g_get_home_dir(), ".codex", "config.toml", NULL);
}

/* Reads a config file that may legitimately not exist yet.
 *
 *   TRUE,  *out == NULL  the file is not there; the caller proceeds as if it
 *                        were empty, which is the first-install case.
 *   TRUE,  *out set      the contents.
 *   FALSE, err set       it is there but could not be read.
 *
 * The two failure shapes must not collapse into one. An unreadable
 * settings.json that read as "missing" would be replaced wholesale by a fresh
 * document — the same file loss the malformed-JSON path already refuses, only
 * quieter, because a permission error or a transient I/O error looks exactly
 * like a user who has never installed anything. */
static gboolean read_user_file(const char *path, char **out, GError **err) {
  GError *local = NULL;
  *out = NULL;
  if (g_file_get_contents(path, out, NULL, &local)) return TRUE;
  if (g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
    g_clear_error(&local);
    return TRUE;
  }
  g_propagate_error(err, local);
  return FALSE;
}

/* The shared refusal: pt read something it was not allowed to read, so it
 * touches nothing. Same posture as the malformed-JSON path. */
static void report_unreadable(const char *path, const GError *err) {
  fprintf(stderr, "pt integration: cannot read %s: %s\n", path, err->message);
  fprintf(stderr, "  refusing to touch a file pt could not read.\n");
}

/* codex's notify program is configured by hand: pt writes the line, the user
 * pastes it. TRUE when the config already names the reporter — the bare
 * subcommand, which matches both the current `pt agent-report` line and the
 * older standalone pt-agent-report one, so an existing install still reads as
 * installed. */
static gboolean codex_installed(const char *config_text) {
  return config_text != NULL && strstr(config_text, "agent-report") != NULL;
}

static int install_claude(const char *pt_bin) {
  char *path = claude_settings_path();
  char *text = NULL;
  GError *err = NULL;
  int rc = 0;

  if (!read_user_file(path, &text, &err)) {
    report_unreadable(path, err);
    rc = 1;
  } else if (pt_integration_claude_installed(text, pt_bin)) {
    printf("claude: already installed in %s\n", path);
  } else {
    char *merged = pt_integration_claude_merged_settings(text, pt_bin, &err);
    if (merged == NULL) {
      /* The one case pt refuses: a settings.json it did not understand. Say
       * what to add instead of guessing at the file. */
      fprintf(stderr, "pt integration: %s\n", err->message);
      fprintf(stderr, "  refusing to rewrite %s — fix it, or add by hand:\n"
                      "    hooks.<SessionStart|Stop|Notification>"
                      " -> [ { \"hooks\": [ { \"type\": \"command\","
                      " \"command\": \"%s agent-report …\" } ] } ]\n",
              path, pt_bin);
      rc = 1;
    } else {
      char *dir = g_path_get_dirname(path);
      if (g_mkdir_with_parents(dir, 0700) != 0) {
        fprintf(stderr, "pt integration: cannot create %s: %s\n", dir,
                g_strerror(errno));
        rc = 1;
      } else if (!g_file_set_contents(path, merged, -1, &err)) {
        fprintf(stderr, "pt integration: cannot write %s: %s\n", path,
                err->message);
        rc = 1;
      } else {
        printf("claude: installed hooks in %s\n", path);
        for (gsize i = 0; i < G_N_ELEMENTS(claude_hooks); i++)
          printf("  %s: %s %s\n", claude_hooks[i].event, pt_bin,
                 claude_hooks[i].args);
      }
      g_free(dir);
      g_free(merged);
    }
  }

  g_clear_error(&err);
  g_free(text);
  g_free(path);
  return rc;
}

/* Print, do not write: config.toml carries comments and ordering that no
 * generic TOML round-trip preserves, and it is the user's file. */
static int install_codex(const char *pt_bin) {
  char *path = codex_config_path();
  char *text = NULL;
  GError *err = NULL;
  int rc = 0;
  /* Refuses even though this branch only prints: without reading the file pt
   * cannot tell whether `notify` is already set, and telling the user to paste
   * a second `notify` key would break their config. */
  if (!read_user_file(path, &text, &err)) {
    report_unreadable(path, err);
    g_clear_error(&err);
    g_free(path);
    return 1;
  }
  if (codex_installed(text)) {
    printf("codex: %s already names pt's agent-report\n", path);
  } else {
    printf("codex: add this line to %s (top level, outside any [section]):\n",
           path);
    printf("\n    notify = [\"%s\", \"agent-report\", \"codex-notify\"]\n\n",
           pt_bin);
    printf("  pt does not edit config.toml itself — it is your file, comments"
           " and all.\n");
  }
  g_free(text);
  g_free(path);
  return rc;
}

/* An unreadable file is reported as such, never as "not installed": that
 * answer would send the user to `install`, which is the write path. */
static int status_line(const char *label, const char *path, gboolean (*probe)(
                           const char *text, const char *arg),
                       const char *arg) {
  char *text = NULL;
  GError *err = NULL;
  if (!read_user_file(path, &text, &err)) {
    printf("%s unreadable (%s): %s\n", label, path, err->message);
    g_clear_error(&err);
    return 1;
  }
  printf("%s %s (%s)\n", label, probe(text, arg) ? "installed"
                                                 : "not installed", path);
  g_free(text);
  return 0;
}

/* One line per event pt installs, so a half-installed state — an older
 * SessionStart-only merge, or a user who deleted one — is visible as exactly
 * which hook is missing. Same readability rule as status_line: unreadable is
 * reported, never answered as "not installed". */
static int claude_status(const char *path, const char *pt_bin) {
  char *text = NULL;
  GError *err = NULL;
  if (!read_user_file(path, &text, &err)) {
    printf("claude unreadable (%s): %s\n", path, err->message);
    g_clear_error(&err);
    return 1;
  }
  for (gsize i = 0; i < G_N_ELEMENTS(claude_hooks); i++)
    printf("claude: %-13s %s (%s)\n", claude_hooks[i].event,
           pt_integration_claude_event_installed(text, pt_bin,
                                                 claude_hooks[i].event)
               ? "installed" : "not installed",
           path);
  g_free(text);
  return 0;
}

static gboolean probe_codex(const char *text, const char *unused) {
  (void)unused;
  return codex_installed(text);
}

static int print_status(const char *pt_bin) {
  char *cpath = claude_settings_path();
  char *xpath = codex_config_path();
  int rc = claude_status(cpath, pt_bin);
  rc |= status_line("codex: ", xpath, probe_codex, NULL);
  g_free(xpath);
  g_free(cpath);
  return rc;
}

static int usage(void) {
  fprintf(stderr,
          "usage: pt integration install claude|codex|all\n"
          "       pt integration status\n"
          "\n"
          "Installs the agent-side hook that reports each pane's session id,\n"
          "so restoring a window can resume the conversation it had.\n");
  return 2;
}

int pt_integration_cli(int argc, char *argv[]) {
  const char *cmd = argc >= 3 ? argv[2] : NULL;
  const char *what = argc >= 4 ? argv[3] : NULL;
  if (cmd == NULL) return usage();

  char *pt_bin = pt_binary_path();
  int rc;
  if (g_strcmp0(cmd, "status") == 0 && what == NULL) {
    rc = print_status(pt_bin);
  } else if (g_strcmp0(cmd, "install") == 0 && g_strcmp0(what, "claude") == 0) {
    rc = install_claude(pt_bin);
  } else if (g_strcmp0(cmd, "install") == 0 && g_strcmp0(what, "codex") == 0) {
    rc = install_codex(pt_bin);
  } else if (g_strcmp0(cmd, "install") == 0 && g_strcmp0(what, "all") == 0) {
    rc = install_claude(pt_bin);
    printf("\n");
    rc |= install_codex(pt_bin);
  } else {
    rc = usage();
  }
  g_free(pt_bin);
  return rc;
}
