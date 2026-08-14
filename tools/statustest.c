/* Resume-without-a-turn must not paint the spinner. That is the session-resume
 * bug: status_resume() used to draw with started==0, so the clock was the Unix
 * timestamp (~500000h) and nothing ticked the frames. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "status.h"
#include "ui.h"

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

static char *slurp_fd(int fd)
{
    char *buf = NULL;
    size_t n = 0, cap = 0;
    char tmp[4096];
    ssize_t r;
    while ((r = read(fd, tmp, sizeof tmp)) > 0) {
        if (n + (size_t)r + 1 > cap) {
            cap = cap ? cap * 2 : 4096;
            while (cap < n + (size_t)r + 1)
                cap *= 2;
            char *grown = realloc(buf, cap);
            if (!grown) {
                free(buf);
                return NULL;
            }
            buf = grown;
        }
        memcpy(buf + n, tmp, (size_t)r);
        n += (size_t)r;
    }
    if (!buf) {
        buf = malloc(1);
        if (!buf)
            return NULL;
    }
    buf[n] = '\0';
    return buf;
}

static char *capture(void (*fn)(void))
{
    int fds[2];
    if (pipe(fds) != 0)
        return NULL;
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    if (saved < 0) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    dup2(fds[1], STDOUT_FILENO);
    close(fds[1]);
    fn();
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    char *out = slurp_fd(fds[0]);
    close(fds[0]);
    return out;
}

static void resume_without_begin(void)
{
    status_set_note("LLM-generated models for 3D printing");
    status_set_word("working");
    status_resume();
    status_tick();
}

static int looks_like_spinner(const char *out)
{
    static const char *const frames[] = {
        "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏",
    };
    if (strstr(out, "working"))
        return 1;
    if (strstr(out, "LLM-generated models"))
        return 1;
    for (size_t i = 0; i < sizeof frames / sizeof *frames; i++)
        if (strstr(out, frames[i]))
            return 1;
    return 0;
}

int main(void)
{
    ui_init();

    if (status_elapsed() != 0)
        fail("elapsed is 0 before begin");

    char *idle = capture(resume_without_begin);
    if (!idle)
        fail("capture resume without begin");
    else if (looks_like_spinner(idle))
        fail("resume without begin painted a spinner");
    free(idle);

    if (status_elapsed() != 0)
        fail("elapsed stays 0 if begin never ran");

    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    int null = open("/dev/null", O_WRONLY);
    if (saved >= 0 && null >= 0)
        dup2(null, STDOUT_FILENO);
    if (null >= 0)
        close(null);
    status_begin();
    double elapsed = status_elapsed();
    status_end();
    if (saved >= 0) {
        fflush(stdout);
        dup2(saved, STDOUT_FILENO);
        close(saved);
    }
    if (elapsed < 0 || elapsed > 2)
        fail("elapsed after begin is the turn clock, not the Unix time");

    if (status_elapsed() != 0)
        fail("elapsed is 0 after end");

    char *after = capture(resume_without_begin);
    if (!after)
        fail("capture resume after end");
    else if (looks_like_spinner(after))
        fail("resume after end painted a spinner");
    free(after);

    if (failures)
        return 1;
    printf("statustest: all checks passed\n");
    return 0;
}
