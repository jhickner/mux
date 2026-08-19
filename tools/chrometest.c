// The chrome is built by two painters that both run during a live turn:
// status.c calls the `above` hook for what sits over the spinner and the
// `below` hook for the input under it, and both land in one block. Anything
// drawn by the wrong one, or by both, shows up twice on screen.
//
// sidechannel is stubbed here rather than linked, so the test can count how
// often the pending /btw rows are asked for per build.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chrome.h"
#include "prompt.h"
#include "restart.h"
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

static int pending_rows;
static int paint_calls;

int sidechannel_rows(void) { return pending_rows; }

void sidechannel_paint(int budget)
{
    (void)budget;
    if (!pending_rows)
        return;
    paint_calls++;
    ui_put("BTWMARK\n");
}

void sidechannel_tick(void) {}
void sidechannel_poll(void) {}
int  sidechannel_busy(void) { return pending_rows > 0; }
void sidechannel_close_all(void) {}
int  sidechannel_fds(int *out, int max) { (void)out; (void)max; return 0; }

void restart_shield_thread(void) {}

/* ------------------------------------------------------------------------- */

int main(void)
{
    setenv("COLUMNS", "80", 1);
    setenv("LINES", "24", 1);

    char path[] = "/tmp/mux-chrometest-XXXXXX";
    int  wfd = mkstemp(path);
    if (wfd < 0) {
        fprintf(stderr, "chrometest: no temp file\n");
        return 1;
    }
    unlink(path);
    fflush(stdout);
    if (dup2(wfd, STDOUT_FILENO) < 0) {
        fprintf(stderr, "chrometest: cannot redirect stdout\n");
        return 1;
    }

    ui_init();
    ui_raw(1);
    viewport_begin();

    struct prompt *p = prompt_new(NULL, 0);
    if (!p) {
        fprintf(stderr, "chrometest: no prompt\n");
        return 1;
    }
    chrome_bind(p);

    // A live turn: status.c drives both painters into one block.
    pending_rows = 1;
    paint_calls = 0;
    status_begin();
    if (paint_calls != 1)
        fail("a pending side turn is painted once per chrome build");
    status_end();

    // With nothing pending, neither painter asks for a row.
    pending_rows = 0;
    paint_calls = 0;
    status_begin();
    if (paint_calls != 0)
        fail("nothing is painted when no side turn is pending");
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
