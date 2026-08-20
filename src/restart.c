#include "restart.h"

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "session.h"
#include "sessionfork.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"

// SIGURG, not SIGUSR1: its default action is to ignore, so signalling every
// mux on the machine cannot kill one built before this handler.
#define RESTART_SIGNAL SIGURG

static volatile sig_atomic_t wanted;
static int                   safe_mode;

static void on_signal(int sig)
{
    (void)sig;
    wanted = 1;
}

void restart_arm(int safe)
{
    safe_mode = safe;

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    // No SA_RESTART: select() should break so a waiting prompt restarts now.
    sigaction(RESTART_SIGNAL, &sa, NULL);

    // An inherited mask would hold it pending for the life of the process.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, RESTART_SIGNAL);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}

// Workers call this so the restart signal only reaches the main thread.
void restart_shield_thread(void)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, RESTART_SIGNAL);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

void restart_request(void)
{
    wanted = 1;
}

int restart_wanted(void)
{
    return wanted != 0;
}

// The session is freed before the exec, so its strings are copied to a pool.
static char  pool[8192];
static size_t pool_used;

static char *arg_copy(const char *s)
{
    size_t n = strlen(s) + 1;
    if (pool_used + n > sizeof pool)
        return NULL;
    char *out = pool + pool_used;
    memcpy(out, s, n);
    pool_used += n;
    return out;
}

int restart_exec(struct session *s)
{
    wanted = 0;
    pool_used = 0;

    char *argv[24];
    int   n = 0;
    argv[n++] = arg_copy(sessionfork_program());
    argv[n++] = "-b";
    argv[n++] = arg_copy(session_backend(s));

    const char *cwd = session_cwd(s);
    if (cwd && *cwd) {
        argv[n++] = "-C";
        argv[n++] = arg_copy(cwd);
    }
    const char *model = session_model(s);
    if (strcmp(model, "default") != 0) {
        argv[n++] = "-m";
        argv[n++] = arg_copy(model);
    }
    const char *effort = session_effort(s);
    if (strcmp(effort, "default") != 0) {
        argv[n++] = "-e";
        argv[n++] = arg_copy(effort);
    }
    if (safe_mode)
        argv[n++] = "-s";

    const char *id = session_id(s);
    if (id && *id && session_can_resume(s)) {
        argv[n++] = "--session";
        argv[n++] = arg_copy(id);
    }
    argv[n] = NULL;

    for (int i = 0; i < n; i++)
        if (!argv[i])
            return 0;

    // Checked before the teardown: past it there is nothing to return to.
    if (strchr(argv[0], '/') && access(argv[0], X_OK) != 0)
        return 0;

    ui_bar(ui_style(UI_DIM), "restarting");
    ui_put("\n");
    ui_flush();

    // The agent CLI is a child: it goes before the exec, or it is orphaned
    // still holding the session.
    session_free(s);
    viewport_end();
    ui_raw(0);
    tty_raw_end();

    execvp(argv[0], argv);

    // The session is already gone, so there is nothing to fall back to.
    ui_raw(0);
    fprintf(stderr, APP_NAME ": could not exec %s\n", argv[0]);
    _exit(1);
}
