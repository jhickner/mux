#include "sessionfork.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "session.h"
#include "ui.h"

#define MAX_FORKS 100

/* Bare when found on PATH, absolute once resolved from argv[0]. */
static char program[4096] = "simple-agent";

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

/* ---------- running things ---------- */

/* Run argv to completion with stdout going to `out_path` (or /dev/null) and
 * stderr discarded — this display owns the terminal. Returns the exit status,
 * or -1 if the child could not be run. */
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

static int capture_line(char *const argv[], char *out, size_t size)
{
    char tmp[] = "/tmp/simple-agent-fork-XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0)
        return 0;
    close(fd);

    int ok = 0;
    if (run(argv, tmp) == 0) {
        FILE *f = fopen(tmp, "r");
        if (f) {
            if (fgets(out, (int)size, f)) {
                out[strcspn(out, "\n")] = '\0';
                ok = *out != '\0';
            }
            fclose(f);
        }
    }
    remove(tmp);
    return ok;
}

/* ---------- the worktree ---------- */

/* Claim the first free fork-N. `git worktree add` is the arbiter: it fails if
 * either the directory or the branch is taken, so a losing name just moves on
 * to the next. */
static int add_worktree(const char *top, char *path, size_t path_size, char *branch,
                        size_t branch_size)
{
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/.claude", top);
    mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/.claude/worktrees", top);
    mkdir(dir, 0700);

    for (int n = 1; n <= MAX_FORKS; n++) {
        snprintf(branch, branch_size, "fork-%d", n);
        snprintf(path, path_size, "%s/fork-%d", dir, n);

        struct stat st;
        if (stat(path, &st) == 0)
            continue;

        char *argv[] = {"git",    "-C",   (char *)top, "worktree", "add",
                        "-b",     branch, path,        "HEAD",     NULL};
        if (run(argv, NULL) == 0)
            return 1;
    }
    return 0;
}

/* Uncommitted tracked changes, replayed into the new worktree so the fork starts
 * from what is on screen rather than from HEAD. Untracked files do not travel. */
static void carry_changes(const char *top, const char *path)
{
    char patch[] = "/tmp/simple-agent-fork-XXXXXX";
    int fd = mkstemp(patch);
    if (fd < 0)
        return;
    close(fd);

    char *diff[] = {"git", "-C", (char *)top, "diff", "HEAD", NULL};
    struct stat st;
    if (run(diff, patch) == 0 && stat(patch, &st) == 0 && st.st_size > 0) {
        char *apply[] = {"git", "-C", (char *)path, "apply", patch, NULL};
        if (run(apply, NULL) != 0)
            ui_note("uncommitted changes did not apply; the fork starts at HEAD");
    }
    remove(patch);
}

/* ---------- interface ---------- */

int sessionfork(const struct session *s, enum fork_where where)
{
    if (!getenv("TMUX")) {
        ui_error("forking needs tmux");
        ui_put("\n");
        return 0;
    }

    /* A fork is a second agent resumed on this conversation, so it needs both a
     * backend that can resume and an id to resume from — which the CLI only
     * reports once a turn has streamed. */
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

    char top[4096];
    char *rev[] = {"git", "-C", (char *)session_cwd(s), "rev-parse", "--show-toplevel", NULL};
    if (!capture_line(rev, top, sizeof top)) {
        ui_error("not a git repository — nothing to make a worktree from");
        ui_put("\n");
        return 0;
    }

    char path[4096], branch[64];
    if (!add_worktree(top, path, sizeof path, branch, sizeof branch)) {
        ui_error("could not create a worktree");
        ui_put("\n");
        return 0;
    }
    carry_changes(top, path);

    char *tmux[20];
    int n = 0;
    tmux[n++] = "tmux";
    if (where == FORK_WINDOW) {
        tmux[n++] = "new-window";
    } else {
        tmux[n++] = "split-window";
        tmux[n++] = where == FORK_SPLIT_H ? "-h" : "-v";
    }
    tmux[n++] = "-c";
    tmux[n++] = path;
    tmux[n++] = program;
    tmux[n++] = "-b";
    tmux[n++] = (char *)session_backend(s);
    tmux[n++] = "--session";
    tmux[n++] = (char *)id;
    /* session_model() reports "default" when no model was asked for, which is a
     * label rather than something to pass back on the command line. */
    const char *model = session_model(s);
    if (strcmp(model, "default") != 0) {
        tmux[n++] = "-m";
        tmux[n++] = (char *)model;
    }
    tmux[n] = NULL;

    if (run(tmux, NULL) != 0) {
        ui_error("tmux would not open the %s", where == FORK_WINDOW ? "window" : "split");
        ui_put("\n");
        return 0;
    }

    ui_bar(ui_style(UI_DIM), "forked \xc2\xb7 %s", branch);
    ui_put("\n");
    ui_flush();
    return 1;
}

/* ---------- the note on the way out ---------- */

void sessionfork_exit_note(const struct session *s)
{
    const char *id = session_id(s);
    if (!id || !*id || !session_can_resume(s))
        return;

    /* Transcripts are per-directory, so the command carries its own cd unless
     * it would land where the shell already is. */
    char here[4096];
    const char *dir = session_cwd(s);
    char lead[4200] = "";
    if (dir && *dir && !(getcwd(here, sizeof here) && !strcmp(here, dir)))
        snprintf(lead, sizeof lead, "cd %s && ", dir);

    /* Only the flags that would not be the defaults on a fresh start. */
    char flags[4200] = "";
    size_t n = 0;
    const char *backend = session_backend(s);
    if (strcmp(backend, "claude") != 0)
        n += (size_t)snprintf(flags + n, sizeof flags - n, " -b %s", backend);
    const char *model = session_model(s);
    if (n < sizeof flags && strcmp(model, "default") != 0)
        snprintf(flags + n, sizeof flags - n, " -m %s", model);

    char cmd[9000];
    snprintf(cmd, sizeof cmd, "%ssimple-agent%s --session %s", lead, flags, id);

    ui_bar(ui_style(UI_DIM), "resume this conversation:");
    /* No gutter bar on the command itself: it has to survive a copy-paste. */
    ui_printf("%s%s%s\n", ui_style(UI_DIM), cmd, ui_style(UI_RESET));
    ui_flush();
}
