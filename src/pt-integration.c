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

/* The one hook event pt needs: Claude Code fires SessionStart with the
 * session id in the payload, which is exactly the resume reference. */
#define CLAUDE_EVENT "SessionStart"

/* Whether any entry under hooks.SessionStart already runs `command`. Also the
 * whole of the "is it installed?" question — installed means precisely that
 * this command is one of the commands that will run. */
static gboolean root_has_command(JsonObject *root, const char *command) {
  JsonObject *hooks = pt_json_obj(root, "hooks");
  if (hooks == NULL) return FALSE;
  JsonArray *event = pt_json_array(hooks, CLAUDE_EVENT);
  if (event == NULL) return FALSE;
  guint n = json_array_get_length(event);
  for (guint i = 0; i < n; i++) {
    JsonNode *node = json_array_get_element(event, i);
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
                                         const char *command) {
  if (settings_text == NULL || command == NULL) return FALSE;
  JsonParser *p = parse_settings(settings_text, NULL);
  if (p == NULL) return FALSE;
  gboolean found = root_has_command(json_node_get_object(json_parser_get_root(p)),
                                    command);
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
                                            const char *command,
                                            GError **err) {
  g_return_val_if_fail(command != NULL && *command != '\0', NULL);

  JsonParser *p = parse_settings(settings_text, err);
  if (p == NULL) return NULL;
  JsonObject *root = json_node_get_object(json_parser_get_root(p));

  /* Already there: hand back the user's own bytes rather than a reformatted
   * copy, so a second install truly changes nothing on disk. */
  if (root_has_command(root, command)) {
    g_object_unref(p);
    return g_strdup(settings_text != NULL ? settings_text : "{}\n");
  }

  JsonObject *hooks = object_member(root, "hooks");
  JsonArray *event = hooks != NULL ? array_member(hooks, CLAUDE_EVENT) : NULL;
  if (event == NULL) {
    g_set_error_literal(err, PT_INTEGRATION_ERROR, 0,
                        "settings.json already uses hooks." CLAUDE_EVENT
                        " in a shape pt does not understand");
    g_object_unref(p);
    return NULL;
  }

  /* One entry, no matcher: SessionStart has no tool to match on. */
  JsonObject *hook = json_object_new();
  json_object_set_string_member(hook, "type", "command");
  json_object_set_string_member(hook, "command", command);
  JsonArray *inner = json_array_new();
  json_array_add_object_element(inner, hook);
  JsonObject *entry = json_object_new();
  json_object_set_array_member(entry, "hooks", inner);
  json_array_add_object_element(event, entry);

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
}

/* ---- the CLI ---- */

/* The pt-agent-report next to this binary, so a build tree, a /usr/local
 * install and a distro package each point the hooks at their own helper.
 * Falls back to the bare name for PATH lookup when the sibling is missing —
 * which is what a split install (pt and pt-agent-report in different dirs)
 * looks like from here. Caller frees. */
static char *helper_path(void) {
  char *exe = g_file_read_link("/proc/self/exe", NULL);
  if (exe != NULL) {
    char *dir = g_path_get_dirname(exe);
    char *helper = g_build_filename(dir, "pt-agent-report", NULL);
    g_free(dir);
    g_free(exe);
    if (g_file_test(helper, G_FILE_TEST_IS_EXECUTABLE)) return helper;
    g_free(helper);
  }
  return g_strdup("pt-agent-report");
}

static char *claude_settings_path(void) {
  return g_build_filename(g_get_home_dir(), ".claude", "settings.json", NULL);
}

static char *codex_config_path(void) {
  return g_build_filename(g_get_home_dir(), ".codex", "config.toml", NULL);
}

/* NULL when the file is not there — which the callers all read as "nothing
 * installed yet", not as an error. */
static char *read_file_or_null(const char *path) {
  char *text = NULL;
  if (!g_file_get_contents(path, &text, NULL, NULL)) return NULL;
  return text;
}

/* codex's notify program is configured by hand: pt writes the line, the user
 * pastes it. TRUE when the config already names the helper. */
static gboolean codex_installed(const char *config_text) {
  return config_text != NULL && strstr(config_text, "pt-agent-report") != NULL;
}

static int install_claude(const char *helper) {
  char *command = g_strconcat(helper, " claude", NULL);
  char *path = claude_settings_path();
  char *text = read_file_or_null(path);
  GError *err = NULL;
  int rc = 0;

  if (pt_integration_claude_installed(text, command)) {
    printf("claude: already installed in %s\n", path);
  } else {
    char *merged = pt_integration_claude_merged_settings(text, command, &err);
    if (merged == NULL) {
      /* The one case pt refuses: a settings.json it did not understand. Say
       * what to add instead of guessing at the file. */
      fprintf(stderr, "pt integration: %s\n", err->message);
      fprintf(stderr, "  refusing to rewrite %s — fix it, or add by hand:\n"
                      "    hooks." CLAUDE_EVENT
                      " -> [ { \"hooks\": [ { \"type\": \"command\","
                      " \"command\": \"%s\" } ] } ]\n", path, command);
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
        printf("claude: installed the " CLAUDE_EVENT " hook in %s\n", path);
        printf("  command: %s\n", command);
      }
      g_free(dir);
      g_free(merged);
    }
  }

  g_clear_error(&err);
  g_free(text);
  g_free(path);
  g_free(command);
  return rc;
}

/* Print, do not write: config.toml carries comments and ordering that no
 * generic TOML round-trip preserves, and it is the user's file. */
static int install_codex(const char *helper) {
  char *path = codex_config_path();
  char *text = read_file_or_null(path);
  if (codex_installed(text)) {
    printf("codex: %s already names pt-agent-report\n", path);
  } else {
    printf("codex: add this line to %s (top level, outside any [section]):\n",
           path);
    printf("\n    notify = [\"%s\", \"codex-notify\"]\n\n", helper);
    printf("  pt does not edit config.toml itself — it is your file, comments"
           " and all.\n");
  }
  g_free(text);
  g_free(path);
  return 0;
}

static int print_status(const char *helper) {
  char *command = g_strconcat(helper, " claude", NULL);
  char *cpath = claude_settings_path();
  char *ctext = read_file_or_null(cpath);
  printf("claude: %s (%s)\n",
         pt_integration_claude_installed(ctext, command) ? "installed"
                                                         : "not installed",
         cpath);
  g_free(ctext);
  g_free(cpath);
  g_free(command);

  char *xpath = codex_config_path();
  char *xtext = read_file_or_null(xpath);
  printf("codex:  %s (%s)\n",
         codex_installed(xtext) ? "installed" : "not installed", xpath);
  g_free(xtext);
  g_free(xpath);
  return 0;
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

  char *helper = helper_path();
  int rc;
  if (g_strcmp0(cmd, "status") == 0 && what == NULL) {
    rc = print_status(helper);
  } else if (g_strcmp0(cmd, "install") == 0 && g_strcmp0(what, "claude") == 0) {
    rc = install_claude(helper);
  } else if (g_strcmp0(cmd, "install") == 0 && g_strcmp0(what, "codex") == 0) {
    rc = install_codex(helper);
  } else if (g_strcmp0(cmd, "install") == 0 && g_strcmp0(what, "all") == 0) {
    rc = install_claude(helper);
    printf("\n");
    rc |= install_codex(helper);
  } else {
    rc = usage();
  }
  g_free(helper);
  return rc;
}
