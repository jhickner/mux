#include "app.h"
#include "status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "block.h"
#include "chrome.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"
#include "text.h"

static const char *const FRAMES[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
#define FRAME_COUNT (COUNT(FRAMES))

static double  started;
static int     active;
static int     visible;
static int     frame;
static double  frame_at;
static char    word[64] = "working";
static char    note[128];

static int              painted;
static int              spin_width;
static int              gap;
static int              dirty;

static int   sticky_on;
static char *sticky_text;
static int   sticky_drawn;
static int   sticky_tracking;
static int   sticky_busy;       /* the turn this prompt asked for is still going */
static unsigned sticky_mark;

static unsigned resize_epoch;
static double   resize_at;
static int      resize_owed;    /* a settled repaint is still to come */

double status_elapsed(void)
{
    if (!active)
        return 0;
    return now_seconds() - started;
}

void status_touch(void) { dirty = 1; }

void status_set_word(const char *text)
{
    char next[sizeof word];
    snprintf(next, sizeof next, "%s", text && *text ? text : "working");
    if (strcmp(next, word) == 0)
        return;
    memcpy(word, next, sizeof word);
    dirty = 1;
}

void status_set_note(const char *text)
{
    char next[sizeof note];
    snprintf(next, sizeof next, "%s", text ? text : "");
    if (strcmp(next, note) == 0)
        return;
    memcpy(note, next, sizeof note);
    dirty = 1;
}

static int size_changing(void)
{
    unsigned epoch = tty_resize_epoch();
    if (epoch != resize_epoch) {
        resize_epoch = epoch;
        resize_at = now_seconds();
        resize_owed = 1;
        dirty = 1;
    }
    if (resize_owed && (now_seconds() - resize_at) * 1000.0 >= TTY_RESIZE_SETTLE_MS) {
        resize_owed = 0;
        // tmux redraws a resizing pane from its own copy, so the frame that
        // ends a resize is sent whole rather than diffed.
        viewport_forget();
        dirty = 1;
        return 0;
    }
    return resize_owed;
}

int spin_advance(int *frame, double *at)
{
    double t = now_seconds();
    if (*at > 0 && (t - *at) * 1000.0 < SPIN_FRAME_MS)
        return 0;
    *at = t;
    (*frame)++;
    return 1;
}

static void erase_block(void)
{
    chrome_clear();
    painted = 0;
}

static void humanize(double seconds, char *out, size_t n)
{
    long total = (long)seconds;
    if (total < 0)
        total = 0;
    if (total < 60)
        snprintf(out, n, "%lds", total);
    else if (total < 3600)
        snprintf(out, n, "%ldm %lds", total / 60, total % 60);
    else
        snprintf(out, n, "%ldh %ldm", total / 3600, (total % 3600) / 60);
}

// Exact: the viewport knows whether the echo's entry is still on screen.
static int sticky_gone(void)
{
    if (!sticky_tracking)
        return 1;
    return !viewport_visible(sticky_mark);
}

#define STICKY_DONE "\xe2\x9c\x93 "
#define STICKY_BUSY "\xe2\x8b\xaf "

static struct ui_wrap sticky_wrap(int measure)
{
    int busy = sticky_busy;
    int cols = ui_columns();
    struct ui_wrap w = {0};
    w.budget = (size_t)(cols - 5 > 4 ? cols - 5 : 4);
    w.gutter = UI_BAR " ";
    w.mark = busy ? STICKY_BUSY : STICKY_DONE;
    w.role = busy ? UI_STICKY : UI_STICKY_DONE;
    w.max_rows = STICKY_LINES;
    w.erase = !measure;
    w.measure = measure;
    return w;
}

static int sticky_showing(void)
{
    return sticky_on && sticky_text && *sticky_text && sticky_gone();
}

int status_sticky_measure(void)
{
    if (!sticky_showing())
        return 0;
    struct ui_wrap w = sticky_wrap(1);
    return ui_wrap_paint(sticky_text, &w);
}

void status_paint_sticky(void)
{
    sticky_drawn = 0;
    if (!sticky_showing())
        return;

    struct ui_wrap w = sticky_wrap(0);
    sticky_drawn = ui_wrap_paint(sticky_text, &w);
}

int status_sticky_rows(void)
{
    return sticky_drawn;
}

void status_paint_spin(void)
{
    char clock[32], left[64];
    humanize(status_elapsed(), clock, sizeof clock);
    snprintf(left, sizeof left, "%s %s", FRAMES[frame % FRAME_COUNT], clock);

    ui_esc(ui_style(UI_SPIN));
    ui_put(left);
    spin_width = (int)ui_cells(left);

    char spin_word[80];
    snprintf(spin_word, sizeof spin_word, " · %s", word[0] ? word : "working");
    int word_width = (int)ui_cells(spin_word);
    if (spin_width + word_width <= ui_columns() - 1) {
        ui_esc(ui_style(UI_DIM));
        ui_put(spin_word);
        spin_width += word_width;
    }

    if (note[0]) {
        char tail[160];
        snprintf(tail, sizeof tail, " · %s", note);
        int width = (int)ui_cells(tail);
        if (spin_width + width <= ui_columns() - 1) {
            ui_esc(ui_style(UI_DIM));
            ui_put(tail);
            spin_width += width;
        }
    }
    ui_esc(ui_style(UI_RESET));
}

int status_spinning(void) { return active && visible; }

int status_gap_row(void) { return gap ? 1 : 0; }

static void paint(void)
{
    chrome_paint();
    painted = 1;
    dirty = 0;
}

static int paint_spin_only(void)
{
    if (!painted || !chrome_paint_spin())
        return 0;
    dirty = 0;
    return 1;
}

void status_begin(void)
{
    started = now_seconds();
    frame = 0;
    frame_at = started;
    active = 1;
    visible = 1;
    gap = 0;
    painted = 0;
    paint();
}

void status_tick(void)
{
    if (!active || !visible || size_changing())
        return;
    int advanced = spin_advance(&frame, &frame_at);
    if (dirty)
        paint();
    else if (advanced && !paint_spin_only())
        paint();
}

void status_pause(void)
{
    if (!active || !visible)
        return;
    erase_block();
    visible = 0;
}

void status_resume(void)
{
    if (!active || visible)
        return;
    visible = 1;
    if (!size_changing())
        paint();
}

void status_gap(int on)
{
    on = on ? 1 : 0;
    if (gap == on)
        return;
    gap = on;
    dirty = 1;
}

void status_sticky_set(int on)
{
    on = on ? 1 : 0;
    if (sticky_on == on)
        return;
    sticky_on = on;
    if (active && visible && !size_changing())
        paint();
}

int status_sticky_enabled(void) { return sticky_on; }

void status_sticky_prompt(const char *text)
{
    free(sticky_text);
    sticky_text = text && *text ? strdup(text) : NULL;
    dirty = 1;

    // The echo is the newest entry, and gone once it scrolls past the top.
    sticky_tracking = 1;
    sticky_mark = viewport_mark() - 1;

    // A prompt is pinned as its turn is sent, and marked done when that turn
    // ends. Reading it off whatever is busy now answers a different question.
    sticky_busy = 1;
}

void status_sticky_erased(void) { sticky_tracking = 0; }

const char *status_sticky_offscreen(void)
{
    if (!sticky_on || !sticky_text)
        return NULL;
    return sticky_gone() ? sticky_text : NULL;
}

void status_end(void)
{
    if (visible)
        erase_block();
    visible = 0;
    active = 0;
    started = 0;
    sticky_busy = 0;
    ui_esc(UI_CURSOR_SHOW);

}
