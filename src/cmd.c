#include "cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "banner.h"
#include "pick.h"
#include "session.h"
#include "sessionfork.h"
#include "sessionlist.h"
#include "settings.h"
#include "ui.h"

const ReplCommand CMD_TABLE[] = {
    {"/new", "start a fresh conversation", NULL},
    {"/clear", "alias for /new", NULL},
    {"/model", "switch model", "[name]"},
    {"/thinking", "show or hide the model's reasoning", "[on|off]"},
    {"/resume", "resume a past conversation", NULL},
    {"/fh", "fork into a horizontal tmux split", NULL},
    {"/fs", "alias for /fh", NULL},
    {"/fv", "fork into a vertical tmux split", NULL},
    {"/fw", "fork into a tmux window", NULL},
    {"/session", "show this session's info and totals", NULL},
    {"/copy", "copy last response to clipboard", NULL},
    {"/help", "show this help", NULL},
    {"/quit", "leave", NULL},
};
const int CMD_COUNT = (int)(sizeof CMD_TABLE / sizeof *CMD_TABLE);

/* The picker only knows Claude's line-up; every other backend takes a name. */
static const struct pick_item MODELS[] = {
    {"claude-opus-5", "most capable"},
    {"claude-opus-5[1m]", "opus with a 1M-token context"},
    {"claude-sonnet-5", "balanced speed and capability"},
    {"claude-haiku-4-5", "fastest"},
    {"claude-fable-5", "compact"},
    {"default", "whatever the claude CLI is configured to use"},
};
#define MODEL_COUNT ((int)(sizeof MODELS / sizeof *MODELS))

static int is_claude(const struct session *s)
{
    return strcmp(session_backend(s), "claude") == 0;
}

/* ---------- helpers ---------- */

static void help_row(const char *label, const char *text)
{
    ui_put("  ");
    ui_put(label);
    for (size_t i = strlen(label); i < 16; i++)
        ui_put(" ");
    ui_esc(ui_style(UI_DIM));
    ui_put(text);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static void help_heading(const char *text)
{
    ui_esc(ui_style(UI_CHROME));
    ui_put(text);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static void show_help(void)
{
    help_heading("commands");
    for (int i = 0; i < CMD_COUNT; i++) {
        char label[32];
        if (CMD_TABLE[i].args)
            snprintf(label, sizeof label, "%s %s", CMD_TABLE[i].name, CMD_TABLE[i].args);
        else
            snprintf(label, sizeof label, "%s", CMD_TABLE[i].name);
        help_row(label, CMD_TABLE[i].desc);
    }
    ui_put("\n");
    help_heading("shortcuts");
    help_row("enter", "submit prompt, or queue it while a turn is running");
    help_row("ctrl-j", "insert a newline");
    help_row("tab", "accept the completion");
    help_row("up / down", "browse history");
    help_row("ctrl-r", "search history");
    help_row("esc", "interrupt the model or a running tool");
    help_row("ctrl-c", "clear the prompt line, or interrupt a running turn");
    help_row("ctrl-d", "quit (on an empty prompt)");
    help_row("ctrl-l", "clear the screen");
    ui_put("\n");
    help_heading("skills");
    help_row("", "your skills, CLAUDE.md, MCP servers and agents load by default.");
    help_row("", "any slash command not listed above goes to the agent CLI, so");
    help_row("", "/w, /todo and the rest work here. start with -s to run without");
    help_row("", "them; /session shows what is active.");
    ui_put("\n");
    ui_flush();
}

static int copy_to_clipboard(const char *text)
{
#if defined(__APPLE__)
    const char *tool = "pbcopy";
#else
    const char *tool = "xclip -selection clipboard 2>/dev/null || wl-copy";
#endif
    FILE *pipe = popen(tool, "w");
    if (!pipe)
        return 0;
    fputs(text, pipe);
    return pclose(pipe) == 0;
}

/* ---------- commands ---------- */

static void do_model(struct session *s, const char *arg)
{
    const char *chosen = arg;
    if ((!chosen || !*chosen) && !is_claude(s)) {
        ui_note("/model <name> — the picker only lists claude's models");
        ui_put("\n");
        ui_flush();
        return;
    }
    if (!chosen || !*chosen) {
        int initial = 0;
        const char *current = session_model(s);
        for (int i = 0; i < MODEL_COUNT; i++)
            if (strcmp(MODELS[i].label, current) == 0)
                initial = i;
        int index = pick("select model", MODELS, MODEL_COUNT, initial);
        if (index < 0)
            return;
        chosen = MODELS[index].label;
    }

    const char *model = strcmp(chosen, "default") == 0 ? NULL : chosen;
    if (!session_set_model(s, model)) {
        ui_error("could not restart on %s", chosen);
        ui_put("\n");
        return;
    }
    banner_identity(s);
    ui_put("\n");
    ui_flush();
}

/* No argument flips it; "on"/"off" set it outright. The choice is remembered
 * across runs. */
static void do_thinking(struct session *s, const char *arg)
{
    int on;
    if (!arg || !*arg)
        on = !session_thinking(s);
    else if (!strcmp(arg, "on"))
        on = 1;
    else if (!strcmp(arg, "off"))
        on = 0;
    else {
        ui_error("/thinking takes on, off, or nothing to flip it");
        ui_put("\n");
        ui_flush();
        return;
    }

    session_set_thinking(s, on);
    settings_set_int(SETTING_THINKING, on);
    ui_note("reasoning %s", on ? "shown" : "hidden");
    ui_put("\n");
    ui_flush();
}

int cmd_resume(struct session *s)
{
    /* The picker reads Claude Code's own transcript store; no other backend
     * keeps one we can list. */
    if (!is_claude(s)) {
        ui_note("%s keeps no transcripts to resume from", session_backend(s));
        ui_put("\n");
        ui_flush();
        return 0;
    }

    struct past_session *list = NULL;
    int count = sessionlist_load(session_cwd(s), session_id(s), &list);
    if (count == 0) {
        ui_note("no past conversations for this directory");
        ui_put("\n");
        return 0;
    }

    struct pick_item *items = calloc((size_t)count, sizeof *items);
    if (!items) {
        free(list);
        return 0;
    }
    for (int i = 0; i < count; i++) {
        items[i].label = list[i].when;
        items[i].detail = list[i].label;
    }

    int resumed = 0;
    int index = pick("resume which conversation", items, count, 0);
    if (index >= 0) {
        if (session_resume(s, list[index].id)) {
            banner_identity(s);
            ui_bar(ui_style(UI_DIM), "resumed \xc2\xb7 %s", list[index].label);
            ui_put("\n");
            resumed = 1;
        } else {
            ui_error("could not resume that conversation");
            ui_put("\n");
        }
    }
    free(items);
    free(list);
    ui_flush();
    return resumed;
}

static void do_new(struct session *s)
{
    if (!session_clear(s)) {
        ui_error("could not clear the conversation");
        ui_put("\n");
        return;
    }
    banner_identity(s);
    ui_bar(ui_style(UI_DIM), "new conversation");
    ui_put("\n");
    ui_flush();
}

static void do_copy(struct session *s)
{
    const char *reply = session_last_reply(s);
    if (!reply) {
        ui_note("nothing to copy yet");
    } else if (copy_to_clipboard(reply)) {
        ui_note("copied %zu characters", strlen(reply));
    } else {
        ui_error("could not reach the clipboard");
    }
    ui_put("\n");
    ui_flush();
}

/* Split "/name rest" into `name` and a pointer to the argument. Returns 0 when
 * the line is not a command this table could own. */
static int split_command(const char *line, char *name, size_t size, const char **arg)
{
    if (*line != '/')
        return 0;

    const char *space = strchr(line, ' ');
    size_t name_len = space ? (size_t)(space - line) : strlen(line);
    if (name_len >= size)
        return 0;

    const char *rest = space ? space + 1 : "";
    while (*rest == ' ')
        rest++;
    *arg = rest;

    memcpy(name, line, name_len);
    name[name_len] = '\0';
    return 1;
}

/* The fork commands and where each one puts the new agent. */
static int fork_target(const char *name, enum fork_where *where)
{
    static const struct {
        const char     *name;
        enum fork_where where;
    } TARGETS[] = {
        {"/fh", FORK_SPLIT_H},
        {"/fs", FORK_SPLIT_H},
        {"/fv", FORK_SPLIT_V},
        {"/fw", FORK_WINDOW},
    };

    for (size_t i = 0; i < sizeof TARGETS / sizeof *TARGETS; i++) {
        if (strcmp(name, TARGETS[i].name) == 0) {
            if (where)
                *where = TARGETS[i].where;
            return 1;
        }
    }
    return 0;
}

/* Forking only reads the session id and the git tree, so it is safe with a turn
 * in flight. Anything that restarts the CLI is not. */
int cmd_is_live(const char *line)
{
    char name[32];
    const char *arg;
    if (!split_command(line, name, sizeof name, &arg))
        return 0;
    return fork_target(name, NULL);
}

void cmd_dispatch_live(struct session *s, const char *line)
{
    char name[32];
    const char *arg;
    enum fork_where where;
    if (split_command(line, name, sizeof name, &arg) && fork_target(name, &where))
        sessionfork(s, where);
}

enum cmd_result cmd_dispatch(struct session *s, const char *line)
{
    char name[32];
    const char *arg;
    enum fork_where where;
    if (!split_command(line, name, sizeof name, &arg))
        return CMD_NOT_A_COMMAND;

    if (!strcmp(name, "/help")) {
        show_help();
    } else if (!strcmp(name, "/new") || !strcmp(name, "/clear")) {
        do_new(s);
    } else if (!strcmp(name, "/model")) {
        do_model(s, arg);
    } else if (!strcmp(name, "/thinking")) {
        do_thinking(s, arg);
    } else if (!strcmp(name, "/resume")) {
        cmd_resume(s);
    } else if (fork_target(name, &where)) {
        sessionfork(s, where);
    } else if (!strcmp(name, "/session")) {
        session_report(s);
    } else if (!strcmp(name, "/copy")) {
        do_copy(s);
    } else if (!strcmp(name, "/quit") || !strcmp(name, "/exit")) {
        return CMD_QUIT;
    } else {
        /* Not ours: hand it to the CLI, which owns the user's skills and its
         * own commands (/w, /todo, /diagram, ...). */
        return CMD_NOT_A_COMMAND;
    }
    return CMD_HANDLED;
}
