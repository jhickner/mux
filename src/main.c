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
#include "prompt.h"
#include "restart.h"
#include "scrollback.h"
#include "session.h"
#include "sessionfork.h"
#include "settings.h"
#include "sidechannel.h"
#include "status.h"
#include "tg.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"
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

static int idle_fds(void *ud, int *out, int max)
{
    int n = 0;
    int fd = session_idle_fd(ud);
    if (fd >= 0 && n < max)
        out[n++] = fd;
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
    sidechannel_poll();
    sidechannel_tick();
    // A chat line waiting is something only the prompt can act on, so the read
    // it is blocked in has to end.
    if (tg_pending())
        tty_wake();
    return session_idle_pump(ud);
}

static char *chat_line(void *ud)
{
    (void)ud;
    return tg_take_line();
}

static int side_busy(void *ud)  { (void)ud; return sidechannel_busy(); }

static void side_tick(void *ud)
{
    (void)ud;
    sidechannel_poll();
    sidechannel_tick();
}
static int idle_busy(void *ud)   { return session_idle_busy(ud); }
static void replay(void *ud)      { session_replay(ud); }
static void blank_line(void *ud)  { hud_print(ud); }

static int restart_pending(void *ud)
{
    (void)ud;
    return restart_wanted();
}

static int idle_restart(void *ud)
{
    // Returns only when the new build could not be run at all, in which case
    // this session keeps going on the old one.
    sidechannel_close_all();
    if (!restart_exec(ud)) {
        ui_error("could not restart — staying on this build");
        ui_put("\n");
        ui_flush();
    }
    return 0;
}

static int echo_filter(void *ud, const char *line)
{
    (void)ud;
    return !cmd_self_echoes(line);
}

static int live_command(void *ud, const char *line)
{
    if (!cmd_is_live(line))
        return 0;
    status_pause();
    if (!cmd_self_echoes(line))
        prompt_echo_message(line);
    cmd_dispatch_live(ud, line);
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
        tg_run(session);
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

    struct prompt *prompt = prompt_new(CMD_TABLE, CMD_COUNT);
    if (!prompt) {
        session_free(session);
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
    prompt_set_live_command(prompt, live_command, session);
    prompt_set_echo_filter(prompt, echo_filter, NULL);
    prompt_set_idle(prompt, idle_fds, idle_render, idle_busy, session);
    prompt_set_restart(prompt, restart_pending, idle_restart, session);
    prompt_set_replay(prompt, replay, session);
    prompt_set_blank(prompt, blank_line, session);
    prompt_set_animate(prompt, side_busy, side_tick, session);
    if (telegram)
        prompt_set_external(prompt, chat_line, NULL);

    ui_put("\n");

    if (resume)
        cmd_resume(session);
    hud_print(session);

    for (;;) {

        offer_project_trust(session);

        char *line = prompt_take_queued(prompt);
        if (line) {
            if (!cmd_self_echoes(line))
                prompt_echo_message(line);
        } else
            line = prompt_read(prompt);
        if (!line)
            break;

        if (bash_is_command(line)) {
            bash_run(line);
            gitinfo_forget();
            char *text = bash_take_context();
            if (text) {
                status_sticky_prompt(line);
                session_turn(session, text);
                cmd_run_deferred(session);
                free(text);
            }
            free(line);
            prompt_restart_check(prompt);
            continue;
        }

        // A line the chat sent runs the same way, but its output has to go back
        // there as well as onto the screen.
        if (prompt_line_was_external(prompt)) {
            tg_run_line(line);
            prompt_restart_check(prompt);
            continue;
        }

        enum cmd_result r = cmd_dispatch(session, line);
        if (r == CMD_QUIT) {
            free(line);
            break;
        }
        if (r == CMD_NOT_A_COMMAND) {
            status_sticky_prompt(line);
            session_turn(session, line);
            cmd_run_deferred(session);
        }
        free(line);
        prompt_restart_check(prompt);
    }

    sidechannel_close_all();
    tg_stop();
    session_set_typeahead(NULL, NULL);
    chrome_bind(NULL);
    prompt_free(prompt);
    viewport_end();
    sessionfork_exit_note(session);
    session_free(session);
    ui_raw(0);
    tty_raw_end();
    return 0;
}
