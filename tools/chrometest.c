// The chrome is one painter composing one stack, in one order:
//
//   gap, pending side turns, sticky prompt, queued lines, blank, spinner, input
//
// Every /btw bug so far has been a section drawn twice, drawn by the wrong
// painter, or left on screen after the thing it described was over. This
// asserts the order and the membership rather than leaving them to be spotted.
//
// sidechannel is stubbed rather than linked: it would drag the session, the
// agent drivers and the markdown renderer in behind it.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chrome.h"
#include "prompt.h"
#include "tty.h"
#include "restart.h"
#include "screenmodel.h"
#include "sidechannel.h"
#include "status.h"
#include "ui.h"
#include "viewport.h"

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

/* --- the stubbed side channel -------------------------------------------- */

static int pending;
static int paint_calls;

int sidechannel_rows(void) { return pending; }

void sidechannel_paint(int budget)
{
    (void)budget;
    for (int i = 0; i < pending; i++) {
        paint_calls++;
        ui_put("BTWROW\n");
    }
}

void sidechannel_tick(void) {}
void sidechannel_poll(void) {}
int  sidechannel_busy(void) { return pending > 0; }
void sidechannel_close_all(void) {}
int  sidechannel_fds(int *out, int max) { (void)out; (void)max; return 0; }

void restart_shield_thread(void) {}

/* ------------------------------------------------------------------------- */

static int tap_read = -1;

static void pump(struct screen *s)
{
    fflush(stdout);
    char    buf[65536];
    ssize_t n;
    while ((n = read(tap_read, buf, sizeof buf)) > 0)
        feed(s, buf, (size_t)n);
}

// One build of the stack, onto a screen that shows nothing yet, so what is
// counted and what is on screen both belong to this paint alone.
static void repaint(struct screen *s)
{
    screen_init(s, 24, 80);
    viewport_forget();
    paint_calls = 0;
    chrome_paint();
    pump(s);
}

static int row_of(const struct screen *s, const char *needle) { return row_with(s, needle); }

// Queued the way the prompt queues one: typed at the live prompt and submitted
// while a turn is running.
static void queue_line(struct prompt *p, const char *text)
{
    for (const char *c = text; *c; c++) {
        tty_event ev = {.key = TK_CHAR, .cp = (uint32_t)(unsigned char)*c, .text = NULL};
        prompt_live_key(p, &ev);
    }
    tty_event enter = {.key = TK_ENTER, .cp = 0, .text = NULL};
    prompt_live_key(p, &enter);
}

// A queued line is a reminder of what is waiting. A long one is cut short, and
// consecutive ones are told apart by a blank row.
static void check_queued(struct prompt *p, struct screen *s)
{
    char lots[512];
    memset(lots, 'q', sizeof lots - 1);
    lots[sizeof lots - 1] = '\0';
    memcpy(lots, "QFIRST", 6);

    queue_line(p, lots);
    queue_line(p, "QSECOND");
    repaint(s);

    int first = row_of(s, "QFIRST");
    int second = row_of(s, "QSECOND");
    if (first < 0)
        fail("a queued line is on screen");
    if (second < 0)
        fail("a second queued line is on screen");
    if (first >= 0 && second >= 0) {
        if (first >= second)
            fail("queued lines are painted in the order they were typed");
        // The first spends its cap, then one blank row, then the second.
        if (second - first != QUEUED_LINES + 1)
            fail("a long queued line is cut short and followed by a blank row");
        if (!row_blank(s, second - 1))
            fail("the row between two queued lines is blank");
    }
    if (row_of(s, "\xe2\x80\xa6") < 0)
        fail("a queued line that was cut short says so");
}

int main(void)
{
    setenv("COLUMNS", "80", 1);
    setenv("LINES", "24", 1);

    char path[] = "/tmp/mux-chrometest-XXXXXX";
    int  wfd = mkstemp(path);
    tap_read = wfd >= 0 ? open(path, O_RDONLY) : -1;
    if (wfd < 0 || tap_read < 0) {
        fprintf(stderr, "chrometest: no temp file\n");
        return 1;
    }
    unlink(path);
    fflush(stdout);
    if (dup2(wfd, STDOUT_FILENO) < 0) {
        fprintf(stderr, "chrometest: cannot redirect stdout\n");
        return 1;
    }
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);

    ui_init();
    ui_raw(1);
    viewport_begin();

    struct prompt *p = prompt_new(NULL, 0);
    if (!p) {
        fprintf(stderr, "chrometest: no prompt\n");
        return 1;
    }
    chrome_bind(p);

    struct screen s;
    screen_init(&s, 24, 80);

    // The sticky prompt only shows once its echo has scrolled away.
    status_sticky_set(1);
    viewport_write("<echo>\n", 7);
    status_sticky_prompt("STICKYTEXT");
    for (int i = 0; i < 60; i++)
        viewport_write("filler\n", 7);

    // A live turn with one question waiting: every section is in play.
    pending = 1;
    status_begin();
    repaint(&s);

    if (paint_calls != 1)
        fail("a pending side turn is painted once per chrome build");

    int sticky = row_of(&s, "STICKYTEXT");
    int btw = row_of(&s, "BTWROW");
    int spin = row_of(&s, "working");

    if (sticky < 0)
        fail("the sticky prompt is on screen");
    if (btw < 0)
        fail("the pending side turn is on screen");
    if (spin < 0)
        fail("the spinner is on screen");

    if (btw >= 0 && sticky >= 0 && btw > sticky)
        fail("the pending side turn sits above the sticky prompt");
    if (sticky >= 0 && spin >= 0 && sticky > spin)
        fail("the sticky prompt sits above the spinner");
    if (btw >= 0 && spin >= 0 && !row_blank(&s, spin - 1))
        fail("a blank row separates what is pinned above from the spinner");

    // Answered: the row goes with it, on the very next paint.
    pending = 0;
    repaint(&s);
    if (paint_calls != 0)
        fail("nothing is painted for a side turn that is over");
    if (row_of(&s, "BTWROW") >= 0)
        fail("an answered side turn leaves no row behind");
    if (row_of(&s, "STICKYTEXT") < 0)
        fail("the sticky prompt outlives the side turn");

    // Two waiting: a row each, and still one pass over the section.
    pending = 2;
    repaint(&s);
    if (paint_calls != 2)
        fail("each pending side turn gets a row of its own");

    // Queued lines: nothing pending, so the section under test stands alone.
    pending = 0;
    check_queued(p, &s);

    status_end();

    chrome_bind(NULL);
    prompt_free(p);
    viewport_end();

    fflush(stdout);
    if (failures)
        return 1;
    fprintf(stderr, "chrometest: all checks passed\n");
    return 0;
}
