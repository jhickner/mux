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
#include "workspace.h"

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

// Flags the new build has to be given again: the argv is rebuilt from the
// session, which knows nothing about the front ends attached to it.
#define RESTART_FLAGS 4
static const char *extra[RESTART_FLAGS];
static int         extra_n;

void restart_flag(const char *flag)
{
    if (extra_n < RESTART_FLAGS)
        extra[extra_n++] = flag;
}

void restart_request(void)
{
    wanted = 1;
}

int restart_wanted(void)
{
    return wanted != 0;
}

void restart_clear(void)
{
    wanted = 0;
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

// What travels to the successor goes through files in the temp directory; it
// unlinks each once it has it. `index` names one of a set, or is negative for
// the only one of its kind.
static int tmp_path(char *out, size_t n, const char *what, int index)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        len--;
    if (index < 0)
        return snprintf(out, n, "%.*s/" APP_NAME "-%s-%ld", (int)len, tmp, what,
                        (long)getpid()) < (int)n;
    return snprintf(out, n, "%.*s/" APP_NAME "-%s-%ld-%d", (int)len, tmp, what,
                    (long)getpid(), index) < (int)n;
}

static int dump_path(char *out, size_t n)
{
    return tmp_path(out, n, "restore", -1);
}

// The installed build, for when the one this process was started from is gone.
static int path_lookup(const char *name, char *out, size_t size)
{
    const char *path = getenv("PATH");
    if (!path || !*path)
        return 0;
    while (*path) {
        const char *end = strchr(path, ':');
        size_t      len = end ? (size_t)(end - path) : strlen(path);
        if (len > 0 && len < size &&
            snprintf(out, size, "%.*s/%s", (int)len, path, name) < (int)size &&
            access(out, X_OK) == 0)
            return 1;
        if (!end)
            break;
        path = end + 1;
    }
    return 0;
}

// The other sessions this window holds. They cannot travel in argv the way the
// one in front does — there may be a dozen — so they go in a file the successor
// reads and opens a tab from, one line each.
static int tabs_path(char *out, size_t n)
{
    return tmp_path(out, n, "tabs", -1);
}

static const char *plain(const char *value)
{
    return value && strcmp(value, "default") ? value : "";
}

static int tabs_dump(const struct session *front, const char *path)
{
    int wrote = 0;
    FILE *f = NULL;

    for (int i = 0; i < workspace_count(); i++) {
        struct session *s = workspace_at(i);
        const char *id = session_id(s);
        // Only a conversation the CLI can pick up again is worth a tab: one
        // with nothing written down yet would come back empty.
        if (s == front || !id || !*id || !session_can_resume(s))
            continue;
        if (!f && !(f = fopen(path, "w")))
            return 0;

        // Its screen travels with it, the way the front one's does: a tab
        // switched to after a restart is the tab that was there, not a replay
        // of the conversation it held.
        char screen[4096];
        if (!tmp_path(screen, sizeof screen, "tab", i) || !workspace_dump(i, screen))
            screen[0] = '\0';

        fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\n", session_backend(s),
                session_cwd(s) ? session_cwd(s) : "", plain(session_model(s)),
                plain(session_effort(s)), id, screen);
        wrote = 1;
    }
    if (f && fclose(f) != 0)
        wrote = 0;
    if (!wrote)
        unlink(path);
    return wrote;
}

int restart_exec(struct session *s)
{
    wanted = 0;
    pool_used = 0;

    char *argv[28];
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
    for (int i = 0; i < extra_n; i++)
        argv[n++] = arg_copy(extra[i]);

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
    // A build launched by path can outlive its directory - a worktree that was
    // merged away - so fall back to whatever is installed on PATH.
    if (strchr(argv[0], '/') && access(argv[0], X_OK) != 0) {
        char found[4096];
        if (!path_lookup(APP_NAME, found, sizeof found))
            return 0;
        char *arg = arg_copy(found);
        if (!arg)
            return 0;
        argv[0] = arg;
    }

    ui_bar(ui_style(UI_DIM), "restarting");
    ui_put("\n");
    ui_flush();

    // The rest of the window travels too: the successor opens a tab for each
    // and puts the screen it had back in it.
    char tabs[4096];
    if (tabs_path(tabs, sizeof tabs) && tabs_dump(s, tabs)) {
        char *arg = arg_copy(tabs);
        if (arg) {
            argv[n++] = "--tabs";
            argv[n++] = arg;
            argv[n] = NULL;
        } else {
            unlink(tabs);
        }
    }

    // Handed over rather than torn down, so the screen is not wiped between
    // the two builds.
    char path[4096];
    int carried = dump_path(path, sizeof path) && viewport_dump(path);
    if (carried) {
        char *arg = arg_copy(path);
        if (arg) {
            argv[n++] = "--restore";
            argv[n++] = arg;
            argv[n] = NULL;
        } else {
            unlink(path);
            carried = 0;
        }
    }

    // Every agent CLI is a child: they go before the exec, or they are
    // orphaned still holding their conversations.
    if (workspace_index_of(s) >= 0)
        workspace_end();
    else
        session_free(s);
    if (carried)
        viewport_handoff();
    else
        viewport_end();
    ui_raw(0);
    tty_raw_end();

    execvp(argv[0], argv);

    // The session is already gone, so there is nothing to fall back to.
    if (carried) {
        unlink(path);
        viewport_end();
    }
    ui_raw(0);
    fprintf(stderr, APP_NAME ": could not exec %s\n", argv[0]);
    _exit(1);
}
