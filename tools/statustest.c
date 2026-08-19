
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "status.h"
#include "ui.h"
#include "viewport.h"

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

// status.c marks the newest entry when the prompt is echoed, so the test has
// to have echoed one for there to be anything to mark.
static void echo_then_mark(const char *text)
{
    // Deliberately not the prompt text: the capture is the whole painted
    // screen, so an echo carrying it could not be told from the sticky copy.
    viewport_write("<echo>\n", 7);
    status_sticky_prompt(text);
}

static void fill_screen(void)
{
    ui_raw(1);
    for (int i = 0; i < 200; i++)
        ui_put("scrollback\n");
    ui_raw(0);
}

static void turn_with_prompt(void)
{
    status_begin();
    status_end();
}

static void check_sticky(void)
{
    status_sticky_set(1);
    if (!status_sticky_enabled())
        fail("floating prompt reports itself on");

    echo_then_mark("summarize notes.txt");
    char *fresh = capture(turn_with_prompt);
    if (!fresh)
        fail("capture with the echo still on screen");
    else if (strstr(fresh, "summarize notes.txt"))
        fail("floating prompt drawn while its echo is still on screen");
    free(fresh);
    if (status_sticky_offscreen())
        fail("floating prompt retained while its echo is still on screen");

    free(capture(fill_screen));
    if (!status_sticky_offscreen())
        fail("floating prompt dropped once its echo scrolled away");
    char *on = capture(turn_with_prompt);
    if (!on)
        fail("capture with the echo scrolled away");
    else if (!strstr(on, "summarize notes.txt"))
        fail("floating prompt missing from the block");
    free(on);

    status_sticky_set(0);
    char *off = capture(turn_with_prompt);
    if (!off)
        fail("capture with the floating prompt off");
    else if (strstr(off, "summarize notes.txt"))
        fail("floating prompt drawn while off");
    free(off);
    if (status_sticky_offscreen())
        fail("floating prompt retained while off");
    status_sticky_set(1);

    echo_then_mark("one\ntwo\nthree");
    free(capture(fill_screen));
    char *fits = capture(turn_with_prompt);
    if (!fits)
        fail("capture a three-line floating prompt");
    else if (status_sticky_rows() != STICKY_LINES)
        fail("a three-line prompt is not drawn in full");
    else if (strstr(fits, "…"))
        fail("a three-line prompt is clipped");
    free(fits);

    echo_then_mark("one\ntwo\nthree\nfour");
    free(capture(fill_screen));
    char *over = capture(turn_with_prompt);
    if (!over)
        fail("capture a four-line floating prompt");
    else if (status_sticky_rows() != STICKY_LINES)
        fail("a four-line prompt is not clipped to three rows");
    else if (!strstr(over, "…"))
        fail("a clipped prompt has no ellipsis");
    else if (strstr(over, "four"))
        fail("a clipped prompt still shows the dropped line");
    free(over);

    char long_prompt[8192];
    memset(long_prompt, 'x', sizeof long_prompt - 1);
    long_prompt[sizeof long_prompt - 1] = '\0';
    echo_then_mark(long_prompt);
    free(capture(fill_screen));
    char *clipped = capture(turn_with_prompt);
    if (!clipped)
        fail("capture a wrapped floating prompt");
    else if (status_sticky_rows() != STICKY_LINES)
        fail("a wrapping prompt is not clipped to three rows");
    else if (!strstr(clipped, "…"))
        fail("a wrapping prompt has no ellipsis");
    free(clipped);
    status_sticky_set(0);
}

int main(void)
{
    setenv("COLUMNS", "80", 1);
    setenv("LINES", "24", 1);

    ui_init();

    // status.c decides the floating prompt against the viewport's rows, so the
    // viewport has to be the thing holding them. Its first paint is swallowed
    // so the test's own output stays readable.
    free(capture(viewport_begin));

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

    check_sticky();

    if (failures)
        return 1;
    printf("statustest: all checks passed\n");
    return 0;
}
