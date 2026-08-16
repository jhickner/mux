#include "bash.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tty.h"
#include "ui.h"

const char *bash_body(const char *line)
{
    if (!line || *line != '!')
        return NULL;
    const char *cmd = line + 1;
    while (*cmd == ' ' || *cmd == '\t')
        cmd++;
    return *cmd ? cmd : NULL;
}

int bash_is_command(const char *line)
{
    return bash_body(line) != NULL;
}

void bash_run(const char *line)
{
    const char *cmd = bash_body(line);
    if (!cmd)
        return;

    /* Cooked mode with the tty inherited, so pagers and full-screen programs
     * behave as they would in a plain shell. */
    int was_raw = tty_is_raw();
    if (was_raw) {
        ui_raw(0);
        ui_cursor_restore();
        tty_raw_end();
    }
    ui_flush();

    /* SIGINT and SIGQUIT belong to the child while it runs, as in system(3). */
    struct sigaction ignore, old_int, old_quit;
    sigemptyset(&ignore.sa_mask);
    ignore.sa_flags = 0;
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGINT, &ignore, &old_int);
    sigaction(SIGQUIT, &ignore, &old_quit);

    int status = 0;
    pid_t pid = fork();
    if (pid == 0) {
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGQUIT, &old_quit, NULL);
        const char *sh = getenv("SHELL");
        if (!sh || !*sh)
            sh = "/bin/sh";
        execl(sh, sh, "-c", cmd, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
    }

    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGQUIT, &old_quit, NULL);

    if (was_raw) {
        tty_raw_begin();
        ui_raw(1);
        ui_cursor_plain();
    }

    if (pid < 0) {
        ui_error("could not run the shell");
    } else if (WIFSIGNALED(status)) {
        ui_error("terminated by signal %d", WTERMSIG(status));
    } else if (WIFEXITED(status) && WEXITSTATUS(status)) {
        ui_error("exit %d", WEXITSTATUS(status));
    }
    ui_put("\n");
    ui_flush();
}
