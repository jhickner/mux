#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agenttabs.h"
#include "app.h"
#include "bash.h"
#include "chrome.h"
#include "cmd.h"
#include "confirm.h"
#include "gitinfo.h"
#include "hud.h"
#include "image.h"
#include "livelist.h"
#include "prompt.h"
#include "restart.h"
#include "handoff.h"
#include "scrollback.h"
#include "session.h"
#include "sessionfork.h"
#include "sessionload.h"
#include "sessionswitch.h"
#include "settings.h"
#include "sidechannel.h"
#include "status.h"
#include "tg.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"
#include "workspace.h"
#include "vendor/agents/backend.h"
#include "vendor/repl.h"
#include "text.h"

static void backend_choices(char *out, size_t size)
{
    size_t n = 0;
    for (const char *const *p = backend_names(); *p && n + 1 < size; p++) {
        int w = snprintf(out + n, size - n, "%s%s", n ? ", " : "", *p);
        if (w > 0)
            n += (size_t)w < size - n ? (size_t)w : size - n - 1;
    }
}

static int backend_known(const char *name)
{
    for (const char *const *p = backend_names(); *p; p++)
        if (strcmp(*p, name) == 0)
            return 1;
    return 0;
}

static void restore_terminal(void)
{
    viewport_end();
    ui_cursor_restore();
    tty_raw_end();
}

static void usage(void)
{
    char choices[128];
    backend_choices(choices, sizeof choices);
    fprintf(stderr,
            "usage: " APP_NAME " [-b backend] [-m model] [-e effort] [-C dir] [-s] [-r] [prompt...]\n"
            "\n"
            "  -b name    agent CLI to drive: %s (default: claude)\n"
            "  -m model   model to run (default: the last /model pick, else the CLI's own)\n"
            "  -e effort  reasoning/thinking effort (default: the last /effort pick, else the CLI's own)\n"
            "  -C dir     working directory for the agent's tools\n"
            "  -s         safe mode: skip skills, CLAUDE.md, MCP servers, hooks\n"
            "  --telegram also answer over Telegram, in the same session\n"
            "  --connect telegram   the same thing, spelled out\n"
            "  -r         --resume: pick a past conversation to continue\n"
            "  --session id  resume a specific conversation (used by the fork commands)\n"
            "  --fork     with --session: branch off it instead of writing back to it\n"
            "  --restore f  take over the screen from a restarting mux (used by /restart)\n"
            "  -h         this help\n"
            "\n"
            "With a prompt on the command line, answer it and exit.\n",
            choices);
}

// Every tab is watched, not just the one on screen: a session left running
// keeps streaming into its own screen while another is in front.
static int idle_fds(void *ud, int *out, int max)
{
    (void)ud;
    int n = workspace_fds(out, max);
    n += sidechannel_fds(out + n, max - n);
    return n + tg_fds(out + n, max - n);
}

static void offer_project_trust(struct session *s)
{
    if (!session_take_trust_request(s))
        return;
    if (confirm_run("trust this folder in codex?") && !session_trust_project(s)) {
        ui_error("could not trust this folder or reload Codex");
        ui_put("\n");
        ui_flush();
    }
}

static int idle_render(void *ud)
{
    (void)ud;
    sidechannel_poll();
    sidechannel_tick();
    // A chat line waiting is something only the prompt can act on, so the read
    // it is blocked in has to end.
    if (tg_pending())
        tty_wake();
    return workspace_pump();
}

static char *chat_line(void *ud)
{
    (void)ud;
    return tg_take_line();
}

static int side_busy(void *ud)  { (void)ud; return sidechannel_busy() || workspace_busy(); }

// A frame's worth of everything that moves while the prompt waits: the
// spinner, the side turns, and whatever the tabs' own turns have produced.
static void side_tick(void *ud)
{
    (void)ud;
    sidechannel_poll();
    sidechannel_tick();
    image_poll();
    workspace_pump();
    status_tick();
}
static int idle_busy(void *ud)   { (void)ud; return workspace_busy(); }
static void replay(void *ud)      { (void)ud; session_replay(workspace_current()); }
static void blank_line(void *ud)  { (void)ud; hud_print(workspace_current()); }
static void switcher(void *ud)    { (void)ud; sessionswitch_run(); }

// The same signal carries a restart and a request for one of this window's
// sessions; which it is depends on whether a request is waiting.
static int takeover_pending(void *ud)
{
    (void)ud;
    return restart_wanted() && handoff_wanted();
}

static void takeover_run(void *ud)
{
    sessionswitch_serve_request();
    restart_clear();
    if (sessionswitch_gave_last())
        prompt_stop(ud);
}

static int restart_pending(void *ud)
{
    (void)ud;
    return restart_wanted() && !handoff_wanted();
}

static int idle_restart(void *ud)
{
    (void)ud;
    // Returns only when the new build could not be run at all, in which case
    // this session keeps going on the old one.
    sidechannel_close_all();
    // Only the session in front travels; the others are closed, and their
    // conversations are in the list to resume from.
    for (int i = workspace_count() - 1; i >= 0; i--)
        if (i != workspace_index())
            workspace_close(i);
    if (!restart_exec(workspace_current())) {
        ui_error("could not restart — staying on this build");
        ui_put("\n");
        ui_flush();
    }
    return 0;
}

static int echo_filter(void *ud, const char *line)
{
    (void)ud;
    if (cmd_self_echoes(line))
        return 0;
    // A line typed behind a running turn is not sent yet, so it is echoed when
    // it is: the transcript keeps the order the agent saw.
    if (session_turn_running(workspace_current()) && !cmd_is_command(line) &&
        !bash_is_command(line))
        return 0;
    return 1;
}

// Escape with nothing typed stops the turn the tab in front is running.
static int cancel_turn(void *ud)
{
    (void)ud;
    struct session *s = workspace_current();
    if (!session_turn_running(s))
        return 0;
    session_interrupt(s);
    return 1;
}

// What a turn leaves for the window to do, drawn into that turn's own screen.
static void turn_done(struct session *s)
{
    cmd_run_deferred(s);
}

static int live_command(void *ud, const char *line)
{
    (void)ud;
    if (!cmd_is_live(line))
        return 0;
    status_pause();
    if (!cmd_self_echoes(line))
        prompt_echo_message(line);
    cmd_dispatch_live(workspace_current(), line);
    status_resume();
    return 1;
}

int main(int argc, char **argv)
{
    static const struct option LONG_OPTS[] = {
        {"backend", required_argument, NULL, 'b'},
        {"model",   required_argument, NULL, 'm'},
        {"effort",  required_argument, NULL, 'e'},
        {"dir",     required_argument, NULL, 'C'},
        {"safe",    no_argument,       NULL, 's'},
        {"resume",  no_argument,       NULL, 'r'},
        {"session", required_argument, NULL, 'S'},
        {"fork",    no_argument,       NULL, 'F'},
        {"restore", required_argument, NULL, 'R'},
        {"telegram", no_argument,      NULL, 'T'},
        {"connect", required_argument, NULL, 'N'},
        {"help",    no_argument,       NULL, 'h'},
        {NULL,      0,                 NULL, 0},
    };

    const char *backend = "claude";
    const char *model = NULL;
    const char *effort = NULL;
    const char *dir = NULL;
    const char *session_arg = NULL;
    const char *restore_arg = NULL;
    int telegram = 0;
    int fork_session = 0;
    int safe_mode = 0;
    int resume = 0;
    int opt;

    while ((opt = getopt_long(argc, argv, "b:m:e:C:srh", LONG_OPTS, NULL)) != -1) {
        switch (opt) {
        case 'b': backend = optarg; break;
        case 'm': model = optarg; break;
        case 'e': effort = optarg; break;
        case 'C': dir = optarg; break;
        case 's': safe_mode = 1; break;
        case 'r': resume = 1; break;
        case 'S': session_arg = optarg; break;
        case 'F': fork_session = 1; break;
        case 'R': restore_arg = optarg; break;
        case 'T': telegram = 1; break;
        case 'N':
            if (strcmp(optarg, "telegram")) {
                fprintf(stderr, APP_NAME ": --connect takes 'telegram'\n");
                return 2;
            }
            telegram = 1;
            break;
        default:  usage(); return opt == 'h' ? 0 : 2;
        }
    }

    if (!backend_known(backend)) {
        char choices[128];
        backend_choices(choices, sizeof choices);
        fprintf(stderr, APP_NAME ": unknown backend '%s' — pick one of %s\n", backend, choices);
        return 2;
    }
    if (effort && !strcmp(effort, "default"))
        effort = NULL;

    sessionfork_set_program(argv[0]);

    if (resume && optind < argc) {
        fprintf(stderr, APP_NAME ": --resume takes no prompt — it starts with the picker\n");
        return 2;
    }

    char cwd[4096];
    if (dir) {
        if (!realpath(dir, cwd)) {
            fprintf(stderr, APP_NAME ": no such directory: %s\n", dir);
            return 1;
        }
    } else if (!getcwd(cwd, sizeof cwd)) {
        fprintf(stderr, APP_NAME ": cannot determine the working directory\n");
        return 1;
    }

    char config[4096];
    int have_config = path_config_dir(config, sizeof config);
    if (have_config) {
        char path[4200];
        snprintf(path, sizeof path, "%s/settings", config);
        settings_open(path);
    }

    if (!model)
        model = session_saved_model(backend);
    if (!effort)
        effort = session_saved_effort(backend);

    ui_init();

    image_init();
    image_set_rows(settings_get_int(SETTING_IMAGE_ROWS, IMAGE_ROWS_DEFAULT));

    int interactive = optind >= argc;

    // With no terminal to share, the chat is the whole front end: no raw mode,
    // no prompt, nothing drawn.
    int chat_only = telegram && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO));
    if (telegram && !interactive) {
        fprintf(stderr, APP_NAME ": --telegram takes no prompt — it is a session\n");
        return 2;
    }
    if (chat_only)
        interactive = 0;

    if (interactive) {
        restart_arm(safe_mode);
        if (tty_raw_begin() != 0) {
            fprintf(stderr, APP_NAME ": not a terminal — pass a prompt as arguments instead\n");
            return 1;
        }
        atexit(restore_terminal);
        ui_raw(1);
        ui_cursor_plain();

        // A restart: the alt screen is already up and holds the last frame.
        if (restore_arg) {
            viewport_inherit();
            scrollback_restore(restore_arg);
            unlink(restore_arg);
        } else {
            viewport_begin();
        }

        // The terminal echoed whatever was typed before raw mode onto the row
        // we are about to draw on. The bytes are queued and reach the prompt.
        if (tty_input_waiting()) {
            ui_esc("\r");
            ui_esc(UI_ERASE_BELOW);
            ui_flush();
        }
    }

    agenttabs_begin(backend);
    if (interactive || chat_only)
        livelist_begin();
    struct session *session = session_new(backend, cwd, model, effort);
    if (session) {
        session_set_customizations(session, !safe_mode);
        session_set_fork(session, fork_session && session_arg);
        session_set_thinking(session, settings_get_int(SETTING_THINKING, 1));
        session_set_compact(session, settings_get_int(SETTING_COMPACT, 0));
        session_set_permission(session,
            session_permission_name(settings_get_int(SETTING_PERMISSION,
                                                     session_permission_default())));

        session_adopt_id(session, session_arg);
    }

    // Before the agent starts: the bridge has its own conventions to teach it,
    // and they are part of the system prompt the process is opened with.
    if (telegram && session && !tg_start(session, chat_only)) {
        if (chat_only) {
            session_free(session);
            return 1;
        }
        telegram = 0;
    }

    if (!session || !session_start(session)) {
        tty_raw_end();
        fprintf(stderr, APP_NAME ": could not start the %s CLI — is it on PATH?\n", backend);
        session_free(session);
        return 1;
    }

    if (chat_only) {
        session_set_naming(session, 0);
        for (;;) {
            tg_run(session);

            // A window asked for this conversation. With no terminal here
            // there is no screen to send with it — only the conversation, and
            // the agent holding it, which has to be let go first.
            char id[128];
            if (!handoff_take_request(id, sizeof id))
                break;
            const char *mine = session_id(session);
            if (!mine || strcmp(mine, id) != 0) {
                handoff_refuse(id);
                continue;
            }
            tg_stop();
            session_free(session);
            handoff_publish(id);
            return 0;
        }
        tg_stop();
        session_free(session);
        return 0;
    }

    if (!interactive) {
        size_t need = 1;
        for (int i = optind; i < argc; i++)
            need += strlen(argv[i]) + 1;
        char *text = calloc(need, 1);
        if (!text) {
            session_free(session);
            return 1;
        }
        size_t at = 0;
        for (int i = optind; i < argc; i++) {
            if (i > optind)
                text[at++] = ' ';
            size_t n = strlen(argv[i]);
            memcpy(text + at, argv[i], n);
            at += n;
        }
        text[at] = '\0';
        session_set_quiet(session, !isatty(STDOUT_FILENO));
        session_set_naming(session, 0);
        int ok = session_turn(session, text);
        free(text);
        session_free(session);
        return ok ? 0 : 1;
    }

    status_sticky_set(settings_get_int(SETTING_STICKY, 0));

    // From here on the window owns a set of sessions rather than one, and the
    // session that was started above is simply the first of them.
    if (!workspace_begin(session, safe_mode)) {
        session_free(session);
        return 1;
    }

    struct prompt *prompt = prompt_new(CMD_TABLE, CMD_COUNT);
    if (!prompt) {
        workspace_end();
        return 1;
    }
    prompt_file_completion(prompt, cwd);
    if (have_config) {
        char history[4200];
        snprintf(history, sizeof history, "%s/history", config);
        prompt_history_open(prompt, history);
    }

    session_set_typeahead(prompt_live_key, prompt);
    chrome_bind(prompt);
    chrome_tabs(workspace_strip_rows, workspace_strip_paint);
    chrome_modal_interrupt(handoff_wanted);
    prompt_set_live_command(prompt, live_command, NULL);
    prompt_set_echo_filter(prompt, echo_filter, NULL);
    prompt_set_idle(prompt, idle_fds, idle_render, idle_busy, NULL);
    prompt_set_restart(prompt, restart_pending, idle_restart, NULL);
    prompt_set_takeover(prompt, takeover_pending, takeover_run, prompt);
    prompt_set_switcher(prompt, switcher, NULL);
    prompt_set_cancel(prompt, cancel_turn, NULL);
    workspace_on_finish(turn_done);
    prompt_set_replay(prompt, replay, NULL);
    prompt_set_blank(prompt, blank_line, NULL);
    prompt_set_animate(prompt, side_busy, side_tick, NULL);
    if (telegram)
        prompt_set_external(prompt, chat_line, NULL);

    ui_put("\n");

    if (!resume || !cmd_resume(session))
        hud_print(session);

    // Started on a conversation that already exists — a fork, or a window
    // opened for one from the command line. What was said in it belongs on the
    // screen; a restart brings its own, which is already up.
    if (!resume && session_arg && !restore_arg)
        sessionload_into(session);

    for (;;) {
        session = workspace_current();
        if (!session)
            break;

        offer_project_trust(session);

        char *line = prompt_take_queued(prompt);
        if (line) {
            if (!cmd_self_echoes(line))
                prompt_echo_message(line);
        } else
            line = prompt_read(prompt);
        if (!line)
            break;

        // The switcher runs inside the read, so the tab this line was typed at
        // is not necessarily the one the loop started on.
        session = workspace_current();
        if (!session) {
            free(line);
            break;
        }

        if (bash_is_command(line)) {
            bash_run(line);
            gitinfo_forget();
            char *text = bash_take_context();
            if (text) {
                // The command is what the sticky prompt shows; what the agent
                // is asked is its output.
                workspace_send(workspace_index(), text, line);
                free(text);
            }
            free(line);
            prompt_restart_check(prompt);
            continue;
        }

        // A line the chat sent runs the same way, but its output has to go back
        // there as well as onto the screen.
        if (prompt_line_was_external(prompt)) {
            // The chat's line runs on this thread, so a turn already in flight
            // at that session has to end first.
            workspace_settle(tg_session());
            tg_run_line(line);
            prompt_restart_check(prompt);
            continue;
        }

        // A command typed behind a running turn either applies now or waits
        // for it, the way one typed during a turn always has.
        if (session_turn_running(session) && cmd_is_command(line) &&
            !cmd_is_quit(line)) {
            cmd_dispatch_live(session, line);
            free(line);
            prompt_restart_check(prompt);
            continue;
        }

        enum cmd_result r = cmd_dispatch(session, line);
        if (r == CMD_QUIT) {
            free(line);
            break;
        }
        if (r == CMD_NOT_A_COMMAND)
            workspace_send(workspace_index(), line, NULL);
        free(line);
        prompt_restart_check(prompt);
    }

    sidechannel_close_all();
    tg_stop();
    session_set_typeahead(NULL, NULL);
    chrome_bind(NULL);
    chrome_tabs(NULL, NULL);
    prompt_free(prompt);
    viewport_end();
    for (int i = 0; i < workspace_count(); i++)
        sessionfork_exit_note(workspace_at(i));
    workspace_end();
    ui_raw(0);
    tty_raw_end();
    return 0;
}
