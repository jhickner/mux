#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "banner.h"
#include "cmd.h"
#include "prompt.h"
#include "session.h"
#include "tty.h"
#include "ui.h"
#include "vendor/claude.h"
#include "vendor/repl.h"

#define APP "simple-agent"

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
    fprintf(stderr,
            "usage: " APP " [-m model] [-C dir] [-s] [-r] [prompt...]\n"
            "\n"
            "  -m model   model to run (default: the claude CLI's own)\n"
            "  -C dir     working directory for the agent's tools\n"
            "  -s         safe mode: skip skills, CLAUDE.md, MCP servers, hooks\n"
            "  -r         --resume: pick a past conversation to continue\n"
            "  -h         this help\n"
            "\n"
            "With a prompt on the command line, answer it and exit.\n");
}

int main(int argc, char **argv)
{
    static const struct option LONG_OPTS[] = {
        {"model",   required_argument, NULL, 'm'},
        {"dir",     required_argument, NULL, 'C'},
        {"safe",    no_argument,       NULL, 's'},
        {"resume",  no_argument,       NULL, 'r'},
        {"help",    no_argument,       NULL, 'h'},
        {NULL,      0,                 NULL, 0},
    };

    const char *model = NULL;
    const char *dir = NULL;
    int safe_mode = 0;
    int resume = 0;
    int opt;

    while ((opt = getopt_long(argc, argv, "m:C:srh", LONG_OPTS, NULL)) != -1) {
        switch (opt) {
        case 'm': model = optarg; break;
        case 'C': dir = optarg; break;
        case 's': safe_mode = 1; break;
        case 'r': resume = 1; break;
        default:  usage(); return opt == 'h' ? 0 : 2;
        }
    }

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

    ui_init();
    struct session *session = session_new(cwd, model);
    if (session)
        session_set_customizations(session, !safe_mode);
    if (!session || !session_start(session)) {
        fprintf(stderr, APP ": could not start the claude CLI — is it on PATH?\n");
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
    ui_cursor_accent();

    struct prompt *prompt = prompt_new(CMD_TABLE, CMD_COUNT);
    if (!prompt) {
        session_free(session);
        return 1;
    }
    char config[4096];
    if (config_dir(config, sizeof config)) {
        char history[4200];
        snprintf(history, sizeof history, "%s/history", config);
        prompt_history_open(prompt, history);
    }

    /* cmd_resume() prints the identity row itself when it adopts a session. */
    if (resume && cmd_resume(session))
        banner_hints();
    else
        banner_print(session);

    for (;;) {
        char *line = prompt_read(prompt);
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

    prompt_free(prompt);
    session_free(session);
    ui_raw(0);
    tty_raw_end();
    return 0;
}
