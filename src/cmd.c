#include "cmd.h"

#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "chrome.h"
#include "fanout.h"
#include "frontend.h"
#include "hud.h"
#include "models.h"
#include "muxcfg.h"
#include "muxmake.h"
#include "pick.h"
#include "prompt.h"
#include "restart.h"
#include "session.h"
#include "image.h"
#include "sessionfork.h"
#include "sessionlist.h"
#include "sessionload.h"
#include "sessionswitch.h"
#include "viewport.h"
#include "sidechannel.h"
#include "settings.h"
#include "settingsui.h"
#include "text.h"
#include "status.h"
#include "ui.h"
#include "vendor/agents/backend.h"

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
static const struct pick_item DEFAULT_EFFORT[] = {
    {"default", "whatever the CLI is configured to use"},
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

const struct pick_item *cmd_model_choices(const char *backend, int *count)
{
    const struct pick_item *v = NULL;
    *count = models_for(backend, &v);
    return v;
}

static const struct pick_item *model_choices(const struct session *s, int *count)
{
    return cmd_model_choices(session_backend(s), count);
}

const struct pick_item *cmd_effort_choices(const char *backend, int *count)
{
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
    *count = COUNT(DEFAULT_EFFORT);
    return DEFAULT_EFFORT;
}

static const struct pick_item *effort_choices(const struct session *s, int *count)
{
    const struct pick_item *v = cmd_effort_choices(session_backend(s), count);
    if (v == DEFAULT_EFFORT)
        *count = 0;
    return *count ? v : NULL;
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
    size_t width = strlen(label);
    for (size_t i = width; i < 16; i++)
        ui_put(" ");
    if (width >= 16)
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

// A picker is a modal on the keyboard, so there has to be one and it has to be
// free: not already inside a list, and not a front end with no keyboard behind
// it. The modal layer refuses on its own too; this is the caller side, where
// there is a usage line to say why.
static int can_pick(const char *usage)
{
    if (chrome_modal_active()) {
        reply_note("%s \xe2\x80\x94 a list is already open", usage);
        return 0;
    }
    if (!frontend_has_keyboard()) {
        reply_note("%s \xe2\x80\x94 nothing here to pick from a list with", usage);
        return 0;
    }
    return 1;
}

static void do_model(struct session *s, const char *arg)
{
    const char *chosen = arg;
    if (!chosen || !*chosen) {
        if (!can_pick("/model <name>"))
            return;
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
        int index = pick_run_filter("select model", choices, count, initial);
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
        if (!can_pick("/effort <level>"))
            return;
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
        if (!can_pick("/permission <mode>"))
            return;
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

static void do_mux(struct session *s, const char *arg);

static void do_mux(struct session *s, const char *arg)
{
    if (arg && !strcmp(arg, "config")) {
        muxcfg_run();
        return;
    }
    if (arg && !strncmp(arg, "make ", 5)) {
        if (muxmake_run(arg + 5))
            do_mux(s, NULL);
        return;
    }
    if (!arg || !*arg) {
        struct mux_spec v[MUX_MAX];
        int             n = muxcfg_load(v, MUX_MAX);
        reply_note("/mux <prompt> — asks the whole matrix at once; "
                   "/mux config to change it, /mux make <what> to have one "
                   "laid out for you");
        ui_note("%s", muxcfg_active());
        ui_put("\n");
        for (int i = 0; i < n; i++) {
            char label[160];
            muxcfg_label(&v[i], label, sizeof label);
            if (*v[i].prompt)
                ui_note("  %s — %s", label, v[i].prompt);
            else
                ui_note("  %s", label);
            ui_put("\n");
        }
        ui_flush();
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
    // The child is asked the question alone; what is shown is what was typed.
    char label[4096];
    snprintf(label, sizeof label, "/btw %s", arg);
    sidechannel_start(s, arg, label);
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

static void do_sticky(struct session *s, const char *arg)
{
    (void)s;
    int on = toggle_arg(arg, "on", "off", status_sticky_enabled(), "/sticky");
    if (on < 0)
        return;

    status_sticky_set(on);
    settings_set_int(SETTING_STICKY, on);
    reply_note("floating prompt %s", on ? "on" : "off");
}

static void do_image(struct session *s, const char *arg)
{
    (void)s;
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
            // The screen was the conversation this one replaces; what belongs
            // here now is what was said in the one being resumed.
            viewport_clear();
            hud_print(s);
            sessionload_into(s);
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

static void do_new(struct session *s, const char *arg)
{
    (void)arg;
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

static void do_copy(struct session *s, const char *arg)
{
    (void)arg;
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

static void do_split(struct session *s, const char *arg)
{
    enum fork_where where = FORK_SPLIT_H;
    if (arg && (*arg == 'v' || *arg == 'V'))
        where = FORK_SPLIT_V;
    else if (arg && (*arg == 'w' || *arg == 'W'))
        where = FORK_WINDOW;
    sessionfork_shell(s, where, 0);
}

static void do_rename(struct session *s, const char *arg)
{
    if (arg && *arg) {
        if (session_rename(s, arg))
            ui_note("renamed to %s", session_title(s));
        else
            ui_error("could not use that name");
    } else if (session_rename(s, NULL)) {
        ui_note("naming this session again");
    } else {
        ui_error("nothing to name it from yet");
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

enum {
    CMD_HIDDEN      = 1u << 0,
    CMD_QUITS       = 1u << 1,
    // The command puts its own line in the transcript.
    CMD_SELF_ECHOES = 1u << 2,
    // Touches only mux's display, so it may run while a turn is in flight.
    // The rest reload or talk to the agent CLI, and wait for the turn to end.
    CMD_LIVE        = 1u << 3,
};

struct cmd {
    const char *name;
    const char *desc;
    const char *args;
    unsigned    flags;
    void      (*run)(struct session *s, const char *arg);
};

static void do_help(struct session *s, const char *arg);

static void do_settings(struct session *s, const char *arg)
{
    (void)arg;
    if (!can_pick("/settings"))
        return;
    settingsui_run(s);
}

static void do_resume(struct session *s, const char *arg)
{
    (void)arg;
    // Only the command form is gated: cmd_resume is also the -r startup path,
    // where the terminal is the front end and does have a keyboard.
    if (!can_pick("/resume"))
        return;
    cmd_resume(s);
}

static void do_sessions(struct session *s, const char *arg)
{
    (void)s;
    (void)arg;
    if (!can_pick("/sessions"))
        return;
    sessionswitch_run();
}

static void do_status(struct session *s, const char *arg)
{
    (void)arg;
    hud_print(s);
}

static void do_session(struct session *s, const char *arg)
{
    (void)arg;
    session_report(s);
}

static void do_restart(struct session *s, const char *arg)
{
    (void)s;
    (void)arg;
    restart_request();
}

static void do_fork_h(struct session *s, const char *arg)
{
    (void)arg;
    sessionfork_run(s, FORK_SPLIT_H);
}

static void do_fork_v(struct session *s, const char *arg)
{
    (void)arg;
    sessionfork_run(s, FORK_SPLIT_V);
}

static void do_fork_w(struct session *s, const char *arg)
{
    (void)arg;
    sessionfork_run(s, FORK_WINDOW);
}

static const struct cmd COMMANDS[] = {
    {"/new", "start a fresh conversation", NULL, 0, do_new},
    {"/clear", "alias for /new", NULL, 0, do_new},
    {"/model", "switch model", "[name]", 0, do_model},
    {"/effort", "set reasoning/thinking effort", "[level]", 0, do_effort},
    {"/backend", "continue with another backend", "<name>", 0, do_backend},
    {"/cd", "work in another directory, starting fresh there", "<path>", 0, do_cd},
    {"/mux", "ask the whole matrix the same thing", "<prompt>|config|make <what>",
     0, do_mux},
    {"/btw", "answer this on the side, without waiting", "<prompt>",
     CMD_SELF_ECHOES | CMD_LIVE, do_btw},
    {"/thinking", "show or hide the model's reasoning", "[on|off]", CMD_LIVE,
     do_thinking},
    {"/tools", "how much of each tool call to show", "[compact|full]", CMD_LIVE,
     do_tools},
    {"/sticky", "float the prompt above the spinner", "[on|off]", CMD_LIVE, do_sticky},
    {"/image", "tallest an inline image may be drawn", "[rows]", CMD_LIVE, do_image},
    {"/permission", "how the CLI gates tool calls", "[mode]", 0, do_permission},
    {"/settings", "show and change every setting", NULL, 0, do_settings},
    {"/resume", "resume a past conversation", NULL, 0, do_resume},
    {"/sessions", "every session: this window's, other windows', past ones", NULL,
     0, do_sessions},
    {"/fh", "fork into a horizontal tmux split", NULL, CMD_LIVE, do_fork_h},
    {"/fs", "alias for /fh", NULL, CMD_LIVE, do_fork_h},
    {"/fv", "fork into a vertical tmux split", NULL, CMD_LIVE, do_fork_v},
    {"/fw", "fork into a tmux window", NULL, CMD_LIVE, do_fork_w},
    {"/split", "a shell split here, in this directory", "[h|v|w]", 0, do_split},
    {"/status", "reprint the status bar", NULL, CMD_LIVE, do_status},
    {"/session", "show this session's info and totals", NULL, CMD_LIVE, do_session},
    {"/rename", "name this session, or ask the model to name it again", "[name]",
     0, do_rename},
    {"/copy", "copy last response to clipboard", NULL, CMD_LIVE, do_copy},
    {"/restart", "reload the mux binary, keeping this conversation", NULL, 0,
     do_restart},
    {"/help", "show this help", NULL, CMD_LIVE, do_help},
    {"/quit", "leave", NULL, CMD_QUITS, NULL},
    {"/exit", "alias for /quit", NULL, CMD_QUITS, NULL},
};

static const struct cmd *cmd_named(const char *name)
{
    for (size_t i = 0; i < COUNT(COMMANDS); i++)
        if (!strcmp(COMMANDS[i].name, name))
            return &COMMANDS[i];
    return NULL;
}

// The command the line names, if any, with the text after its name.
static const struct cmd *cmd_for_line(const char *line, const char **arg)
{
    char name[32];
    if (!split_command(line, name, sizeof name, arg))
        return NULL;
    return cmd_named(name);
}

const ReplCommand *cmd_completions(int *count)
{
    static ReplCommand table[COUNT(COMMANDS)];
    static int         n;

    if (!n) {
        for (size_t i = 0; i < COUNT(COMMANDS); i++) {
            if (COMMANDS[i].flags & CMD_HIDDEN)
                continue;
            table[n].name = COMMANDS[i].name;
            table[n].desc = COMMANDS[i].desc;
            table[n].args = COMMANDS[i].args;
            n++;
        }
    }
    *count = n;
    return table;
}

int cmd_is_command(const char *line)
{
    const char *arg;
    return cmd_for_line(line, &arg) != NULL;
}

int cmd_is_quit(const char *line)
{
    const char *arg;
    const struct cmd *c = cmd_for_line(line, &arg);
    return c && (c->flags & CMD_QUITS);
}

// /btw shows the question itself, twice over; the echo would be a third copy.
int cmd_self_echoes(const char *line)
{
    const char *arg;
    const struct cmd *c = cmd_for_line(line, &arg);
    return c && (c->flags & CMD_SELF_ECHOES) && arg && *arg;
}

// Every command but a quit goes through cmd_dispatch_live() while a turn is
// running: it either runs there and then or waits for the turn to end.
int cmd_runs_mid_turn(const char *line)
{
    const char *arg;
    const struct cmd *c = cmd_for_line(line, &arg);
    return c && !(c->flags & CMD_QUITS);
}

#define DEFERRED_MAX 8
static struct {
    char           *line;
    struct session *s;
} deferred[DEFERRED_MAX];
static int   deferred_count;

void cmd_dispatch_live(struct session *s, const char *line)
{
    const char       *arg;
    const struct cmd *c = cmd_for_line(line, &arg);
    if (!c || (c->flags & CMD_QUITS))
        return;

    if (c->flags & CMD_LIVE) {
        c->run(s, arg);
        return;
    }

    if (deferred_count == DEFERRED_MAX) {
        reply_error("too many settings changes are already waiting");
        return;
    }
    char *copy = strdup(line);
    if (!copy) {
        reply_error("could not hold %s until the turn ends", c->name);
        return;
    }
    deferred[deferred_count].line = copy;
    deferred[deferred_count].s = s;
    deferred_count++;
    reply_note("%s applies when this turn ends", c->name);
}

// Only the ones typed at this session: another tab's turn ending is not the
// moment to change this one's model.
void cmd_run_deferred(struct session *s)
{
    char *mine[DEFERRED_MAX];
    int   n = 0, kept = 0;

    for (int i = 0; i < deferred_count; i++) {
        if (deferred[i].s == s)
            mine[n++] = deferred[i].line;
        else
            deferred[kept++] = deferred[i];
    }
    deferred_count = kept;

    for (int i = 0; i < n; i++) {
        cmd_dispatch(s, mine[i]);
        free(mine[i]);
    }
}

// The session is going away: its held-back commands will never be drained by
// cmd_run_deferred(), and a later session could land on the same address.
void cmd_forget_session(struct session *s)
{
    int kept = 0;
    for (int i = 0; i < deferred_count; i++) {
        if (deferred[i].s == s)
            free(deferred[i].line);
        else
            deferred[kept++] = deferred[i];
    }
    deferred_count = kept;
}

enum cmd_result cmd_dispatch(struct session *s, const char *line)
{
    const char       *arg;
    const struct cmd *c = cmd_for_line(line, &arg);
    if (!c)
        return CMD_NOT_A_COMMAND;
    if (c->flags & CMD_QUITS)
        return CMD_QUIT;
    c->run(s, arg);
    return CMD_HANDLED;
}

static void do_help(struct session *s, const char *arg)
{
    (void)s;
    (void)arg;

    help_heading("commands");
    for (size_t i = 0; i < COUNT(COMMANDS); i++) {
        if (COMMANDS[i].flags & CMD_HIDDEN)
            continue;
        char label[64];
        if (COMMANDS[i].args)
            snprintf(label, sizeof label, "%s %s", COMMANDS[i].name, COMMANDS[i].args);
        else
            snprintf(label, sizeof label, "%s", COMMANDS[i].name);
        help_row(label, COMMANDS[i].desc);
    }
    ui_put("\n");
    help_heading("shortcuts");
    // The shell escape is main.c's line dispatch, not a prompt key; the rest
    // come from the prompt, which keeps them beside the code that acts on them.
    help_row("!cmd", "run cmd in $SHELL instead of sending it to the agent");
    int                      key_count = 0;
    const struct prompt_key *keys = prompt_shortcuts(&key_count);
    for (int i = 0; i < key_count; i++)
        help_row(keys[i].key, keys[i].desc);
    ui_put("\n");
    help_heading("skills");
    help_row("", "your skills, CLAUDE.md, MCP servers and agents load by default.");
    help_row("", "any slash command not listed above goes to the agent CLI, so");
    help_row("", "/w, /todo and the rest work here. start with -s to run without");
    help_row("", "them; /session shows what is active.");
    ui_put("\n");
    ui_flush();
}
