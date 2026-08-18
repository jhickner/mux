#include "cmd.h"

#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "fanout.h"
#include "hud.h"
#include "pick.h"
#include "prompt.h"
#include "restart.h"
#include "session.h"
#include "image.h"
#include "sessionfork.h"
#include "sessionlist.h"
#include "sidechannel.h"
#include "settings.h"
#include "text.h"
#include "status.h"
#include "ui.h"
#include "vendor/agents/backend.h"

const ReplCommand CMD_TABLE[] = {
    {"/new", "start a fresh conversation", NULL},
    {"/clear", "alias for /new", NULL},
    {"/model", "switch model", "[name]"},
    {"/effort", "set reasoning/thinking effort", "[level]"},
    {"/backend", "continue with another backend", "<name>"},
    {"/cd", "work in another directory, starting fresh there", "<path>"},
    {"/mux", "ask several backends the same thing", "<prompt>"},
    {"/btw", "answer this on the side, without waiting", "<prompt>"},
    {"/thinking", "show or hide the model's reasoning", "[on|off]"},
    {"/tools", "how much of each tool call to show", "[compact|full]"},
    {"/sticky", "float the prompt above the spinner", "[on|off]"},
    {"/image", "tallest an inline image may be drawn", "[rows]"},
    {"/permission", "how the CLI gates tool calls", "[mode]"},
    {"/resume", "resume a past conversation", NULL},
    {"/fh", "fork into a horizontal tmux split", NULL},
    {"/fs", "alias for /fh", NULL},
    {"/fv", "fork into a vertical tmux split", NULL},
    {"/fw", "fork into a tmux window", NULL},
    {"/status", "reprint the status bar", NULL},
    {"/session", "show this session's info and totals", NULL},
    {"/copy", "copy last response to clipboard", NULL},
    {"/restart", "reload the mux binary, keeping this conversation", NULL},
    {"/help", "show this help", NULL},
    {"/quit", "leave", NULL},
};
const int CMD_COUNT = COUNT(CMD_TABLE);

static const struct pick_item MODELS[] = {
    {"claude-opus-5", "most capable"},
    {"claude-opus-5[1m]", "opus with a 1M-token context"},
    {"claude-sonnet-5", "balanced speed and capability"},
    {"claude-haiku-4-5", "fastest"},
    {"claude-fable-5", "compact"},
    {"default", "whatever the claude CLI is configured to use"},
};
#define MODEL_COUNT (COUNT(MODELS))

static const struct pick_item GROK_MODELS[] = {
    {"grok-4.6", "latest frontier model"},
    {"grok-4.5", "the prior generation"},
    {"default", "whatever the grok CLI is configured to use"},
};
#define GROK_MODEL_COUNT (COUNT(GROK_MODELS))

static const struct pick_item CLAUDE_EFFORTS[] = {
    {"default", "auto: use the model's default effort"},
    {"low", "faster, lighter reasoning"},
    {"medium", "balanced reasoning"},
    {"high", "more thorough reasoning"},
    {"xhigh", "extra-high reasoning"},
    {"max", "maximum reasoning"},
    {"ultracode", "xhigh effort with dynamic workflow orchestration"},
};
static const struct pick_item CODEX_EFFORTS[] = {
    {"default", "whatever Codex is configured to use"},
    {"none", "no reasoning"},
    {"low", "faster, lighter reasoning"},
    {"medium", "balanced reasoning"},
    {"high", "more thorough reasoning"},
    {"xhigh", "extra-high reasoning"},
    {"max", "maximum reasoning"},
};
static const struct pick_item GROK_EFFORTS[] = {
    {"default", "GROK_EFFORT or the CLI default"},
    {"low", "faster, lighter reasoning"},
    {"medium", "balanced reasoning"},
    {"high", "more thorough reasoning"},
    {"xhigh", "extra-high reasoning, grok-4.6 only"},
};
static const struct pick_item PI_EFFORTS[] = {
    {"default", "the thinking level active when pi started"},
    {"off", "no thinking"},
    {"minimal", "minimal thinking"},
    {"low", "faster, lighter thinking"},
    {"medium", "balanced thinking"},
    {"high", "more thorough thinking"},
    {"xhigh", "extra-high thinking, when the model supports it"},
    {"max", "maximum thinking, when the model supports it"},
};

static int is_claude(const struct session *s)
{
    return strcmp(session_backend(s), "claude") == 0;
}

static const struct pick_item *model_choices(const struct session *s, int *count)
{
    const char *backend = session_backend(s);
    if (!strcmp(backend, "claude")) {
        *count = MODEL_COUNT;
        return MODELS;
    }
    if (!strcmp(backend, "grok")) {
        *count = GROK_MODEL_COUNT;
        return GROK_MODELS;
    }
    *count = 0;
    return NULL;
}

static const struct pick_item *effort_choices(const struct session *s, int *count)
{
    const char *backend = session_backend(s);
    if (!strcmp(backend, "claude")) {
        *count = COUNT(CLAUDE_EFFORTS);
        return CLAUDE_EFFORTS;
    }
    if (!strcmp(backend, "codex")) {
        *count = COUNT(CODEX_EFFORTS);
        return CODEX_EFFORTS;
    }
    if (!strcmp(backend, "grok")) {
        *count = COUNT(GROK_EFFORTS);
        return GROK_EFFORTS;
    }
    if (!strcmp(backend, "pi")) {
        *count = COUNT(PI_EFFORTS);
        return PI_EFFORTS;
    }
    *count = 0;
    return NULL;
}

__attribute__((format(printf, 2, 3)))
static void reply(int error, const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);

    if (error)
        ui_error("%s", text);
    else
        ui_note("%s", text);
    ui_put("\n");
    ui_flush();
}

#define reply_note(...)  reply(0, __VA_ARGS__)
#define reply_error(...) reply(1, __VA_ARGS__)

static int known_backend(const char *name)
{
    for (const char *const *p = backend_names(); name && *p; p++)
        if (strcmp(name, *p) == 0)
            return 1;
    return 0;
}

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
    help_row("!cmd", "run cmd in $SHELL instead of sending it to the agent");
    help_row("enter", "submit prompt, or queue it while a turn is running");
    help_row("ctrl-j", "insert a newline");
    help_row("ctrl-g", "edit the prompt in $EDITOR");
    help_row("tab", "accept the completion");
    help_row("@", "complete a file path from the working directory");
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
    int ok = fputs(text, pipe) != EOF;
    return pclose(pipe) == 0 && ok;
}

static void note_identity(const struct session *s)
{
    char effort[64] = "", ctx[32] = "";
    if (session_can_set_effort(s))
        snprintf(effort, sizeof effort, " \xc2\xb7 %s effort", session_effort(s));

    long window = session_context_window(s);
    if (window >= 1000000)
        snprintf(ctx, sizeof ctx, " \xc2\xb7 %.3gM context", (double)window / 1e6);
    else if (window > 0)
        snprintf(ctx, sizeof ctx, " \xc2\xb7 %ldK context", window / 1000);

    ui_note("%s \xc2\xb7 %s%s%s", session_backend(s),
            session_model_short(s, session_model_label(s)), effort, ctx);
    ui_put("\n");
    ui_flush();
}

static void do_model(struct session *s, const char *arg)
{
    const char *chosen = arg;
    if (!chosen || !*chosen) {
        int count = 0, initial = 0;
        const struct pick_item *choices = model_choices(s, &count);
        if (!count) {
            reply_note("/model <name> — %s has no model list here", session_backend(s));
            return;
        }
        const char *current = session_model(s);
        for (int i = 0; i < count; i++)
            if (strcmp(choices[i].label, current) == 0)
                initial = i;
        int index = pick_run("select model", choices, count, initial);
        if (index < 0)
            return;
        chosen = choices[index].label;
    }

    const char *model = strcmp(chosen, "default") == 0 ? NULL : chosen;
    if (!session_set_model(s, model)) {
        ui_error("could not restart on %s", chosen);
        return;
    }
    note_identity(s);
}

static void do_effort(struct session *s, const char *arg)
{
    if (!session_can_set_effort(s)) {
        reply_note("%s does not support changing effort", session_backend(s));
        return;
    }

    const char *chosen = arg;
    if (!chosen || !*chosen) {
        int count = 0, initial = 0;
        const struct pick_item *choices = effort_choices(s, &count);
        const char *current = session_effort(s);
        for (int i = 0; i < count; i++)
            if (!strcmp(choices[i].label, current))
                initial = i;
        int index = pick_run("set effort", choices, count, initial);
        if (index < 0)
            return;
        chosen = choices[index].label;
    }

    const char *effort = !strcmp(chosen, "default") ? NULL : chosen;
    if (!session_set_effort(s, effort)) {
        reply_error("could not set effort to %s", chosen);
        return;
    }
    note_identity(s);
}

static void do_backend(struct session *s, const char *arg)
{
    if (!arg || !*arg) {
        reply_note("/backend <claude|codex|grok|pi>");
        return;
    }
    if (!known_backend(arg)) {
        reply_error("unknown backend '%s'", arg);
        return;
    }
    if (strcmp(arg, session_backend(s)) == 0) {
        reply_note("already using %s", arg);
        return;
    }

    char *from = strdup(session_backend(s));
    const char *failed = session_failed_prompt(s);
    char *retry = failed ? strdup(failed) : NULL;
    if (!from || (failed && !retry)) {
        free(from);
        free(retry);
        reply_error("could not prepare the backend handoff");
        return;
    }
    if (!session_switch_backend(s, arg)) {
        reply_error("could not start %s; still using %s", arg, from);
        free(from);
        free(retry);
        return;
    }

    hud_print(s);
    free(from);

    if (retry) {
        reply_note("retrying the failed turn with %s", arg);
        ui_put("\n\n");
        ui_flush();
        session_turn(s, retry);
        free(retry);
    }
}

static void do_permission(struct session *s, const char *arg)
{
    if (!is_claude(s)) {
        ui_note("/permission only applies to claude");
        return;
    }

    int count = session_permission_count();
    struct pick_item choices[16];
    if (count > (int)COUNT(choices))
        count = (int)COUNT(choices);
    for (int i = 0; i < count; i++) {
        choices[i].label = session_permission_name(i);
        choices[i].detail = session_permission_desc(i);
    }

    const char *chosen = arg;
    if (!chosen || !*chosen) {
        int initial = session_permission_index(session_permission(s));
        int index = pick_run("gate tool calls", choices, count, initial < 0 ? 0 : initial);
        if (index < 0)
            return;
        chosen = choices[index].label;
    }

    int index = session_permission_index(chosen);
    if (index < 0) {
        reply_error("unknown mode '%s'", chosen);
        return;
    }

    if (!session_set_permission(s, session_permission_name(index))) {
        reply_error("could not restart in %s", chosen);
        return;
    }
    settings_set_int(SETTING_PERMISSION, index);
    reply_note("tool calls: %s", session_permission_desc(index));
}

static void do_mux(struct session *s, const char *arg)
{
    if (!arg || !*arg) {
        reply_note("/mux <prompt> — asks %s the same thing at once",
                   settings_get_str(SETTING_MUX_BACKENDS, "claude, codex, grok"));
        return;
    }
    fanout_run(s, arg);
}

static void do_btw(struct session *s, const char *arg)
{
    if (!arg || !*arg) {
        reply_note("/btw <prompt> — answers on a one-turn fork of this "
                   "conversation, without waiting for the current turn");
        return;
    }
    sidechannel_start(s, arg);
    ui_flush();
}

static int toggle_arg(const char *arg, const char *on_word, const char *off_word,
                      int current, const char *command)
{
    if (!arg || !*arg)
        return !current;
    if (!strcmp(arg, on_word))
        return 1;
    if (!strcmp(arg, off_word))
        return 0;
    reply_error("%s takes %s, %s, or nothing to flip it", command, on_word, off_word);
    return -1;
}

static void do_thinking(struct session *s, const char *arg)
{
    int on = toggle_arg(arg, "on", "off", session_thinking(s), "/thinking");
    if (on < 0)
        return;

    session_set_thinking(s, on);
    settings_set_int(SETTING_THINKING, on);
    reply_note("reasoning %s", on ? "shown" : "hidden");
}

static void do_tools(struct session *s, const char *arg)
{
    int compact = toggle_arg(arg, "compact", "full", session_compact(s), "/tools");
    if (compact < 0)
        return;

    session_set_compact(s, compact);
    settings_set_int(SETTING_COMPACT, compact);
    reply_note("tool calls: %s", compact ? "one row each" : "full blocks with output");
}

static void do_sticky(const char *arg)
{
    int on = toggle_arg(arg, "on", "off", status_sticky_enabled(), "/sticky");
    if (on < 0)
        return;

    status_sticky_set(on);
    settings_set_int(SETTING_STICKY, on);
    reply_note("floating prompt %s", on ? "on" : "off");
}

static void do_image(const char *arg)
{
    if (arg && *arg) {
        char *end;
        long rows = strtol(arg, &end, 10);
        while (*end == ' ')
            end++;
        if (*end || rows < IMAGE_ROWS_MIN || rows > IMAGE_ROWS_MAX) {
            reply_error("/image takes a row count between %d and %d",
                     IMAGE_ROWS_MIN, IMAGE_ROWS_MAX);
            return;
        }
        image_set_rows((int)rows);
        settings_set_int(SETTING_IMAGE_ROWS, image_rows());
    }

    if (image_available())
        reply_note("inline images: up to %d rows tall", image_rows());
    else
        ui_note("inline images: up to %d rows tall, but this terminal has no "
                "graphics support", image_rows());
    ui_put("\n");
    ui_flush();
}

int cmd_resume(struct session *s)
{

    if (!sessionlist_available(session_backend(s))) {
        ui_note("%s keeps no transcripts to resume from", session_backend(s));
        return 0;
    }

    struct past_session *list = NULL;
    int count = sessionlist_load(session_backend(s), session_cwd(s), session_id(s),
                                 &list);
    if (count == 0) {
        reply_note("no past conversations for this directory");
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
    int index = pick_run("resume which conversation", items, count, 0);
    if (index >= 0) {
        if (session_resume(s, list[index].id)) {
            status_sticky_prompt(NULL);
            ui_bar(ui_style(UI_DIM), "resumed \xc2\xb7 %s", list[index].label);
            ui_put("\n");
            resumed = 1;
        } else {
            reply_error("could not resume that conversation");
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
        reply_error("could not clear the conversation");
        return;
    }

    status_sticky_prompt(NULL);
    ui_bar(ui_style(UI_DIM), "new conversation");
    ui_put("\n");
    ui_flush();
}

static void do_cd(struct session *s, const char *arg)
{
    char shown[4096];
    if (!arg || !*arg) {
        path_home_relative(session_cwd(s), shown, sizeof shown);
        reply_note("%s", shown);
        return;
    }

    char *expanded = path_expand_home(arg);
    char  resolved[4096];
    const char *want = expanded ? expanded : arg;
    int ok = realpath(want, resolved) != NULL;
    free(expanded);
    if (!ok) {
        reply_error("no such directory: %s", arg);
        return;
    }
    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) {
        reply_error("not a directory: %s", arg);
        return;
    }
    if (!strcmp(resolved, session_cwd(s))) {
        path_home_relative(resolved, shown, sizeof shown);
        reply_note("already in %s", shown);
        return;
    }

    if (!session_set_cwd(s, resolved)) {
        reply_error("could not start %s in %s", session_backend(s), resolved);
        return;
    }
    if (chdir(resolved) != 0)
        reply_error("the agent moved, but mux could not follow");
    prompt_rehome(resolved);

    status_sticky_prompt(NULL);
    path_home_relative(resolved, shown, sizeof shown);
    ui_bar(ui_style(UI_DIM), "new conversation in %s", shown);
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

// What a command can do while a turn is in flight. NOW covers the ones that
// only touch mux's own display or spawn something of their own; LATER covers
// the ones that reload or talk to the agent CLI, which would trample the turn,
// so they are acknowledged straight away and applied once it ends.
enum live_class { LIVE_NO, LIVE_NOW, LIVE_LATER };

static enum live_class live_class(const char *name)
{
    static const char *const NOW[] = {
        "/help", "/status", "/session", "/copy", "/thinking",
        "/tools", "/sticky", "/image", "/btw",
    };
    static const char *const LATER[] = {"/model", "/effort", "/permission"};

    for (size_t i = 0; i < COUNT(NOW); i++)
        if (!strcmp(name, NOW[i]))
            return LIVE_NOW;
    for (size_t i = 0; i < COUNT(LATER); i++)
        if (!strcmp(name, LATER[i]))
            return LIVE_LATER;
    return fork_target(name, NULL) ? LIVE_NOW : LIVE_NO;
}

#define DEFERRED_MAX 8
static char *deferred[DEFERRED_MAX];
static int   deferred_count;

int cmd_is_live(const char *line)
{
    char name[32];
    const char *arg;
    if (!split_command(line, name, sizeof name, &arg))
        return 0;
    return live_class(name) != LIVE_NO;
}

static enum cmd_result run_command(struct session *s, const char *name,
                                   const char *arg);

void cmd_dispatch_live(struct session *s, const char *line)
{
    char name[32];
    const char *arg;
    if (!split_command(line, name, sizeof name, &arg))
        return;

    if (live_class(name) == LIVE_NOW) {
        run_command(s, name, arg);
        return;
    }

    if (deferred_count == DEFERRED_MAX) {
        reply_error("too many settings changes are already waiting");
        return;
    }
    char *copy = strdup(line);
    if (!copy) {
        reply_error("could not hold %s until the turn ends", name);
        return;
    }
    deferred[deferred_count++] = copy;
    reply_note("%s applies when this turn ends", name);
}

void cmd_run_deferred(struct session *s)
{
    int count = deferred_count;
    deferred_count = 0;
    for (int i = 0; i < count; i++) {
        cmd_dispatch(s, deferred[i]);
        free(deferred[i]);
        deferred[i] = NULL;
    }
}

enum cmd_result cmd_dispatch(struct session *s, const char *line)
{
    char name[32];
    const char *arg;
    if (!split_command(line, name, sizeof name, &arg))
        return CMD_NOT_A_COMMAND;
    return run_command(s, name, arg);
}

static enum cmd_result run_command(struct session *s, const char *name,
                                   const char *arg)
{
    enum fork_where where;

    if (!strcmp(name, "/help")) {
        show_help();
    } else if (!strcmp(name, "/new") || !strcmp(name, "/clear")) {
        do_new(s);
    } else if (!strcmp(name, "/model")) {
        do_model(s, arg);
    } else if (!strcmp(name, "/effort")) {
        do_effort(s, arg);
    } else if (!strcmp(name, "/backend")) {
        do_backend(s, arg);
    } else if (!strcmp(name, "/cd")) {
        do_cd(s, arg);
    } else if (!strcmp(name, "/mux")) {
        do_mux(s, arg);
    } else if (!strcmp(name, "/btw")) {
        do_btw(s, arg);
    } else if (!strcmp(name, "/thinking")) {
        do_thinking(s, arg);
    } else if (!strcmp(name, "/image")) {
        do_image(arg);
    } else if (!strcmp(name, "/tools")) {
        do_tools(s, arg);
    } else if (!strcmp(name, "/sticky")) {
        do_sticky(arg);
    } else if (!strcmp(name, "/permission")) {
        do_permission(s, arg);
    } else if (!strcmp(name, "/resume")) {
        cmd_resume(s);
    } else if (fork_target(name, &where)) {
        sessionfork_run(s, where);
    } else if (!strcmp(name, "/status")) {
        hud_print(s);
    } else if (!strcmp(name, "/session")) {
        session_report(s);
    } else if (!strcmp(name, "/copy")) {
        do_copy(s);
    } else if (!strcmp(name, "/restart")) {
        restart_request();
    } else if (!strcmp(name, "/quit") || !strcmp(name, "/exit")) {
        return CMD_QUIT;
    } else {

        return CMD_NOT_A_COMMAND;
    }
    return CMD_HANDLED;
}
