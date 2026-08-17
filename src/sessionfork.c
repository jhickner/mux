#include "sessionfork.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app.h"
#include "session.h"
#include "ui.h"

static char program[4096] = APP_NAME;

void sessionfork_set_program(const char *argv0)
{
    if (!argv0 || !*argv0)
        return;
    char resolved[4096];
    if (strchr(argv0, '/') && realpath(argv0, resolved))
        snprintf(program, sizeof program, "%s", resolved);
    else
        snprintf(program, sizeof program, "%s", argv0);
}

const char *sessionfork_program(void)
{
    return program;
}

static int run(char *const argv[], const char *out_path)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int out = out_path ? open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600)
                           : open("/dev/null", O_WRONLY);
        if (out >= 0) {
            dup2(out, STDOUT_FILENO);
            close(out);
        }
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, STDERR_FILENO);
            close(null);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int sessionfork_run(const struct session *s, enum fork_where where)
{
    if (!getenv("TMUX")) {
        ui_error("forking needs tmux");
        ui_put("\n");
        return 0;
    }

    if (!session_can_resume(s)) {
        ui_error("%s cannot resume a conversation, so there is nothing to fork",
                 session_backend(s));
        ui_put("\n");
        return 0;
    }
    const char *id = session_id(s);
    if (!id || !*id) {
        ui_error("nothing to fork yet — send a message first");
        ui_put("\n");
        return 0;
    }

    char *tmux[20];
    int n = 0;
    tmux[n++] = "tmux";
    if (where == FORK_WINDOW) {
        tmux[n++] = "new-window";
    } else {
        tmux[n++] = "split-window";
        tmux[n++] = where == FORK_SPLIT_H ? "-h" : "-v";
    }

    const char *cwd = session_cwd(s);
    if (cwd) {
        tmux[n++] = "-c";
        tmux[n++] = (char *)cwd;
    }
    tmux[n++] = program;
    tmux[n++] = "-b";
    tmux[n++] = (char *)session_backend(s);
    tmux[n++] = "--session";
    tmux[n++] = (char *)id;

    const char *model = session_model(s);
    if (strcmp(model, "default") != 0) {
        tmux[n++] = "-m";
        tmux[n++] = (char *)model;
    }
    const char *effort = session_effort(s);
    if (strcmp(effort, "default") != 0) {
        tmux[n++] = "-e";
        tmux[n++] = (char *)effort;
    }
    tmux[n] = NULL;

    if (run(tmux, NULL) != 0) {
        ui_error("tmux would not open the %s", where == FORK_WINDOW ? "window" : "split");
        ui_put("\n");
        return 0;
    }

    ui_bar(ui_style(UI_DIM), "forked");
    ui_put("\n");
    ui_flush();
    return 1;
}

void sessionfork_exit_note(const struct session *s)
{
    const char *id = session_id(s);
    if (!id || !*id || !session_can_resume(s))
        return;

    char here[4096];
    const char *dir = session_cwd(s);
    char lead[4200] = "";
    if (dir && *dir && !(getcwd(here, sizeof here) && !strcmp(here, dir)))
        snprintf(lead, sizeof lead, "cd %s && ", dir);

    char flags[4200] = "";
    size_t n = 0;
    const char *backend = session_backend(s);
    if (strcmp(backend, "claude") != 0)
        n += (size_t)snprintf(flags + n, sizeof flags - n, " -b %s", backend);
    const char *model = session_model(s);
    if (n < sizeof flags && strcmp(model, "default") != 0)
        n += (size_t)snprintf(flags + n, sizeof flags - n, " -m %s", model);
    const char *effort = session_effort(s);
    if (n < sizeof flags && strcmp(effort, "default") != 0)
        snprintf(flags + n, sizeof flags - n, " -e %s", effort);

    char cmd[9000];
    snprintf(cmd, sizeof cmd, "%s" APP_NAME "%s --session %s", lead, flags, id);

    ui_bar(ui_style(UI_DIM), "resume this conversation:");

    ui_printf("%s%s%s\n", ui_style(UI_DIM), cmd, ui_style(UI_RESET));
    ui_flush();
}
