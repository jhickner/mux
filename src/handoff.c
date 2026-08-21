#include "handoff.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "livelist.h"
#include "text.h"

// How long the asker waits. A window mid-turn answers when the turn's stream
// next comes up for air, which is why this is seconds rather than milliseconds.
#define WAIT_MS   15000
#define POLL_MS   30

static int dir_path(char *out, size_t size)
{
    char base[4096];
    if (!path_config_dir(base, sizeof base))
        return 0;
    if ((size_t)snprintf(out, size, "%s/handoff", base) >= size)
        return 0;
    return mkdir(out, 0700) == 0 || errno == EEXIST;
}

// A session id is a file name here, so anything that could leave the directory
// disqualifies it.
static int id_ok(const char *id)
{
    if (!id || !*id || strlen(id) > 120)
        return 0;
    for (const char *p = id; *p; p++)
        if (*p == '/' || *p == '.' || *p < 0x20)
            return 0;
    return 1;
}

static int leaf(const char *name, const char *ext, char *out, size_t size)
{
    char dir[4200];
    if (!dir_path(dir, sizeof dir))
        return 0;
    return (size_t)snprintf(out, size, "%s/%s%s", dir, name, ext) < size;
}

static int request_path(long pid, char *out, size_t size)
{
    char name[32];
    snprintf(name, sizeof name, "%ld", pid);
    return leaf(name, ".req", out, size);
}

int handoff_screen_path(const char *id, char *out, size_t size)
{
    return id_ok(id) && leaf(id, ".state.tmp", out, size);
}

static int state_path(const char *id, char *out, size_t size)
{
    return id_ok(id) && leaf(id, ".state", out, size);
}

static int refused_path(const char *id, char *out, size_t size)
{
    return id_ok(id) && leaf(id, ".no", out, size);
}

int handoff_wanted(void)
{
    char path[4400];
    struct stat st;
    return request_path((long)getpid(), path, sizeof path) && stat(path, &st) == 0;
}

int handoff_take_request(char *id, size_t size)
{
    char path[4400];
    if (!request_path((long)getpid(), path, sizeof path))
        return 0;

    size_t len = 0;
    char *text = text_slurp(path, 4096, &len);
    unlink(path);
    if (!text)
        return 0;
    text_chomp(text);
    snprintf(id, size, "%s", text);
    free(text);
    return id_ok(id);
}

void handoff_refuse(const char *id)
{
    char path[4400];
    if (!refused_path(id, path, sizeof path))
        return;
    FILE *f = fopen(path, "w");
    if (f)
        fclose(f);
}

int handoff_publish(const char *id)
{
    char tmp[4400], path[4400];
    if (!handoff_screen_path(id, tmp, sizeof tmp) || !state_path(id, path, sizeof path))
        return 0;

    // A handover with no screen to carry still has to be announced, so the
    // marker is written either way.
    struct stat st;
    if (stat(tmp, &st) != 0) {
        FILE *f = fopen(tmp, "w");
        if (!f)
            return 0;
        fclose(f);
    }
    return rename(tmp, path) == 0;
}

static void nap(void)
{
    struct timespec ts = {0, POLL_MS * 1000000L};
    nanosleep(&ts, NULL);
}

int handoff_ask(long pid, const char *id, char *screen, size_t size,
                void (*tick)(int waited_ms, void *ud), void *ud)
{
    char req[4400], state[4400], no[4400];
    if (!id_ok(id) || !request_path(pid, req, sizeof req) ||
        !state_path(id, state, sizeof state) || !refused_path(id, no, sizeof no))
        return 0;

    // Anything left from an earlier attempt would read as an instant answer.
    unlink(state);
    unlink(no);

    char tmp[4500];
    snprintf(tmp, sizeof tmp, "%s.tmp", req);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return 0;
    int ok = fprintf(f, "%s\n", id) > 0;
    if (fclose(f) != 0 || !ok || rename(tmp, req) != 0) {
        unlink(tmp);
        return 0;
    }

    // The same signal a restart travels on: the window tells the two apart by
    // whether a request is waiting for it.
    if (kill((pid_t)pid, SIGURG) != 0) {
        unlink(req);
        return 0;
    }

    struct stat st;
    for (int waited = 0; waited < WAIT_MS; waited += POLL_MS) {
        if (stat(state, &st) == 0) {
            snprintf(screen, size, "%s", state);
            return 1;
        }
        if (stat(no, &st) == 0) {
            unlink(no);
            return 0;
        }
        if (!livelist_alive(pid))
            break;
        if (tick)
            tick(waited, ud);
        nap();
    }

    unlink(req);
    // It may have let go as the wait ran out.
    if (stat(state, &st) == 0) {
        snprintf(screen, size, "%s", state);
        return 1;
    }
    return 0;
}
