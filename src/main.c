#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agenttabs.h"
#include "banner.h"
#include "cmd.h"
#include "prompt.h"
#include "session.h"
#include "sessionfork.h"
#include "settings.h"
#include "status.h"
#include "tty.h"
#include "ui.h"
#include "vendor/agents/backend.h"
#include "vendor/repl.h"

#define APP "simple-agent"

/* The agent CLIs backend.h can drive, as "claude, codex, grok, pi". */
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

/* ~/.config/simple-agent, created on demand. Returns 0 when HOME is unset. */
static int config_dir(char *out, size_t size)
{
    const char *home = getenv("HOME");
    if (!home)
        return 0;
    snprintf(out, size, "%s/.config/%s", home, APP);
    mkdir(out, 0700);
    return 1;
}

static void usage(void)
{
    char choices[128];
    backend_choices(choices, sizeof choices);
    fprintf(stderr,
            "usage: " APP " [-b backend] [-m model] [-e effort] [-C dir] [-s] [-r] [prompt...]\n"
            "\n"
            "  -b name    agent CLI to drive: %s (default: claude)\n"
            "  -m model   model to run (default: the CLI's own)\n"
            "  -e effort  reasoning/thinking effort (default: the CLI's own)\n"
            "  -C dir     working directory for the agent's tools\n"
            "  -s         safe mode: skip skills, CLAUDE.md, MCP servers, hooks\n"
            "  -r         --resume: pick a past conversation to continue\n"
            "  --session id  resume a specific conversation (used by the fork commands)\n"
            "  -h         this help\n"
            "\n"
            "With a prompt on the command line, answer it and exit.\n",
            choices);
}

/* The context gauge in front of the caret. */
static const char *context_gauge(void *ud)
{
    static char text[8];
    int percent = session_context_percent(ud);
    if (percent < 0)
        return NULL;
    snprintf(text, sizeof text, "%d%%", percent);
    return text;
}

/* A command submitted while a turn is streaming. The spinner and the live
 * prompt block are lifted out of the way so the command's own output lands in
 * scrollback rather than being overwritten. */
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
        {"session", required_argument, NULL, 'S'}, /* long only: no -S */
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
        fprintf(stderr, APP ": unknown backend '%s' — pick one of %s\n", backend, choices);
        return 2;
    }
    if (effort && !strcmp(effort, "default"))
        effort = NULL;

    sessionfork_set_program(argv[0]);

    /* The picker needs the terminal, so it cannot combine with one-shot mode. */
    if (resume && optind < argc) {
        fprintf(stderr, APP ": --resume takes no prompt — it starts with the picker\n");
        return 2;
    }

    char cwd[4096];
    if (dir) {
        if (!realpath(dir, cwd)) {
            fprintf(stderr, APP ": no such directory: %s\n", dir);
            return 1;
        }
    } else if (!getcwd(cwd, sizeof cwd)) {
        fprintf(stderr, APP ": cannot determine the working directory\n");
        return 1;
    }

    char config[4096];
    int have_config = config_dir(config, sizeof config);
    if (have_config) {
        char path[4200];
        snprintf(path, sizeof path, "%s/settings", config);
        settings_open(path);
    }

    ui_init();
    /* Before session_start(): the CLI it spawns inherits the marker that keeps
     * the plugin's own hook from reporting this pane too. */
    agenttabs_begin(backend);
    struct session *session = session_new(backend, cwd, model, effort);
    if (session) {
        session_set_customizations(session, !safe_mode);
        session_set_thinking(session, settings_get_int(SETTING_THINKING, 1));
        session_set_compact(session, settings_get_int(SETTING_COMPACT, 0));
        session_set_permission(session,
            session_permission_name(settings_get_int(SETTING_PERMISSION, 0)));
        /* Set before start: the CLI then comes up already resumed. */
        session_adopt_id(session, session_arg);
    }
    if (!session || !session_start(session)) {
        fprintf(stderr, APP ": could not start the %s CLI — is it on PATH?\n", backend);
        return 1;
    }

    /* One-shot mode: everything after the options is a single prompt. */
    if (optind < argc) {
        size_t need = 1;
        for (int i = optind; i < argc; i++)
            need += strlen(argv[i]) + 1;
        char *text = calloc(need, 1);
        if (!text)
            return 1;
        for (int i = optind; i < argc; i++) {
            if (i > optind)
                strcat(text, " ");
            strcat(text, argv[i]);
        }
        session_set_quiet(session, !isatty(STDOUT_FILENO));
        int ok = session_turn(session, text);
        free(text);
        session_free(session);
        return ok ? 0 : 1;
    }

    if (tty_raw_begin() != 0) {
        fprintf(stderr, APP ": not a terminal — pass a prompt as arguments instead\n");
        session_free(session);
        return 1;
    }
    atexit(restore_terminal);
    ui_raw(1);
    ui_cursor_plain();

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

    /* Keep the prompt on screen and editable while a turn runs, so a message
     * typed then is queued for the moment the turn ends. */
    session_set_typeahead(prompt_live_key, prompt);
    status_set_below(prompt_live_paint, prompt_live_offset, prompt);
    prompt_set_live_command(prompt, live_command, session);
    prompt_set_status(prompt, context_gauge, session);

    /* cmd_resume() prints the identity row itself when it adopts a session. */
    if (resume && cmd_resume(session))
        banner_hints();
    else
        banner_print(session);

    for (;;) {
        /* Messages queued during the last turn run first, in the order typed. */
        char *line = prompt_take_queued(prompt);
        if (line)
            prompt_echo_message(line);
        else
            line = prompt_read(prompt);
        if (!line)
            break;

        enum cmd_result r = cmd_dispatch(session, line);
        if (r == CMD_QUIT) {
            free(line);
            break;
        }
        if (r == CMD_NOT_A_COMMAND && !session_turn(session, line)) {
            free(line);
            break;
        }
        free(line);
    }

    /* Both hooks borrow the prompt, so they go first. */
    session_set_typeahead(NULL, NULL);
    status_set_below(NULL, NULL, NULL);
    prompt_free(prompt);
    sessionfork_exit_note(session);
    session_free(session);
    ui_raw(0);
    tty_raw_end();
    return 0;
}
