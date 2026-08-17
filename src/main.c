#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agenttabs.h"
#include "app.h"
#include "bash.h"
#include "cmd.h"
#include "confirm.h"
#include "gitinfo.h"
#include "hud.h"
#include "image.h"
#include "prompt.h"
#include "session.h"
#include "sessionfork.h"
#include "settings.h"
#include "status.h"
#include "tty.h"
#include "ui.h"
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
            "  -r         --resume: pick a past conversation to continue\n"
            "  --session id  resume a specific conversation (used by the fork commands)\n"
            "  -h         this help\n"
            "\n"
            "With a prompt on the command line, answer it and exit.\n",
            choices);
}

static int idle_fd(void *ud)     { return session_idle_fd(ud); }
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
    return session_idle_pump(ud);
}
static int idle_busy(void *ud)   { return session_idle_busy(ud); }
static void replay(void *ud)      { session_replay(ud); }

static int live_command(void *ud, const char *line)
{
    if (!cmd_is_live(line))
        return 0;
    status_pause();
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
        {"help",    no_argument,       NULL, 'h'},
        {NULL,      0,                 NULL, 0},
    };

    const char *backend = "claude";
    const char *model = NULL;
    const char *effort = NULL;
    const char *dir = NULL;
    const char *session_arg = NULL;
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

    if (interactive) {
        if (tty_raw_begin() != 0) {
            fprintf(stderr, APP_NAME ": not a terminal — pass a prompt as arguments instead\n");
            return 1;
        }
        atexit(restore_terminal);
        ui_raw(1);
        ui_cursor_plain();

        // Whatever was typed before raw mode landed was echoed by the terminal
        // onto the line we are about to draw on. Wipe it: the bytes themselves
        // are still queued and surface at the prompt once it opens.
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
        session_set_thinking(session, settings_get_int(SETTING_THINKING, 1));
        session_set_compact(session, settings_get_int(SETTING_COMPACT, 0));
        session_set_permission(session,
            session_permission_name(settings_get_int(SETTING_PERMISSION, 0)));

        session_adopt_id(session, session_arg);
    }
    if (!session || !session_start(session)) {
        tty_raw_end();
        fprintf(stderr, APP_NAME ": could not start the %s CLI — is it on PATH?\n", backend);
        session_free(session);
        return 1;
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
    status_set_below(prompt_live_paint, prompt_live_offset, prompt);
    prompt_set_live_command(prompt, live_command, session);
    prompt_set_idle(prompt, idle_fd, idle_render, idle_busy, session);
    prompt_set_replay(prompt, replay, session);

    ui_put("\n");

    if (resume)
        cmd_resume(session);
    hud_print(session);

    for (;;) {

        offer_project_trust(session);

        char *line = prompt_take_queued(prompt);
        if (line)
            prompt_echo_message(line);
        else
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
                free(text);
            }
            free(line);
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
        }
        free(line);
    }

    session_set_typeahead(NULL, NULL);
    status_set_below(NULL, NULL, NULL);
    prompt_free(prompt);
    sessionfork_exit_note(session);
    session_free(session);
    ui_raw(0);
    tty_raw_end();
    return 0;
}
