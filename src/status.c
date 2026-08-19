#include "app.h"
#include "status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "block.h"
#include "tty.h"
#include "ui.h"
#include "text.h"

static const char *const FRAMES[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
#define FRAME_COUNT (COUNT(FRAMES))

#define FRAME_MS 90

static double  started;
static int     active;
static int     visible;
static int     frame;
static double  frame_at;
static char    word[64] = "working";
static char    note[128];

static status_paint_fn  below;
static void            *below_ud;
static status_above_fn  above;
static void            *above_ud;
static int              painted;
static int              spin_width;
static int              gap;
static int              dirty;

static int   sticky_on;
static char *sticky_text;
static int   sticky_drawn;
static int   sticky_tracking;
static int   sticky_row;
static unsigned sticky_scrolls;

static int   spin_row;

// The block is painted onto absolute rows, so it must not be taller than the
// screen. Each paint hands the optional chrome a row budget and it yields once
// the budget is spent.
static int   chrome_budget;

static unsigned resize_epoch;
static double   resize_at;

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

void status_set_below(status_paint_fn paint_fn, void *ud)
{
    below = paint_fn;
    below_ud = ud;
}

void status_set_above(status_above_fn paint_fn, void *ud)
{
    above = paint_fn;
    above_ud = ud;
}

static int size_changing(void)
{
    unsigned epoch = tty_resize_epoch();
    if (epoch != resize_epoch) {
        resize_epoch = epoch;
        resize_at = now_seconds();
        dirty = 1;
    }
    return resize_at > 0 && (now_seconds() - resize_at) * 1000.0 < TTY_RESIZE_SETTLE_MS;
}

static void erase_block(void)
{
    block_clear();
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

int status_rows_left(void)
{
    int left = chrome_budget - ui_sink_rows();
    return left > 0 ? left : 0;
}

// The echo of the prompt was painted one row above the transcript cursor; it is
// off screen once that many screen scrolls have gone by.
static int sticky_gone(void)
{
    if (!sticky_tracking)
        return 1;
    return sticky_row - (int)(block_scrolls() - sticky_scrolls) < 1;
}

static void paint_sticky(void)
{
    sticky_drawn = 0;
    if (!sticky_on || !sticky_text || !*sticky_text || !sticky_gone())
        return;

    int cols = ui_columns();

    struct ui_wrap w = {0};
    w.budget = (size_t)(cols - 3 > 4 ? cols - 3 : 4);
    w.gutter = UI_BAR " ";
    w.role = UI_STICKY;
    w.max_rows = STICKY_LINES;
    w.erase = 1;

    struct ui_wrap m = w;
    m.measure = 1;
    m.erase = 0;
    if (ui_wrap_paint(sticky_text, &m) + 1 > status_rows_left())
        return;

    sticky_drawn = ui_wrap_paint(sticky_text, &w);

    ui_esc(UI_ERASE_EOL);
    ui_put("\n");
}

int status_sticky_rows(void)
{
    return sticky_drawn;
}

static void paint_spin(void)
{
    char clock[32], left[64];
    humanize(status_elapsed(), clock, sizeof clock);
    snprintf(left, sizeof left, "%s %s", FRAMES[frame], clock);

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

static void paint(void)
{
    if (ui_too_narrow()) {
        erase_block();
        return;
    }

    block_begin();

    chrome_budget = tty_rows() - 3;

    if (gap)
        ui_put("\n");
    paint_sticky();

    if (above)
        above(above_ud);

    spin_row = ui_sink_rows();
    paint_spin();

    int row = spin_row, col = -1;
    if (below) {
        int rows = 1, at = 0, at_col = 0;
        ui_put("\n");
        int first = ui_sink_rows();
        below(below_ud, &rows, &at, &at_col);
        row = first + at;
        col = at_col;
    }
    block_end(row, col);
    painted = 1;
    dirty = 0;
}

static int paint_spin_only(void)
{
    if (!painted || !below || ui_columns() < 24 || !block_have())
        return 0;

    block_row_begin(spin_row);
    paint_spin();
    block_row_end();
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
    double t = now_seconds();
    int advanced = 0;
    if ((t - frame_at) * 1000.0 >= FRAME_MS) {
        frame = (frame + 1) % FRAME_COUNT;
        frame_at = t;
        advanced = 1;
    }
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

    sticky_tracking = 1;
    sticky_row = block_out_row() - 1;
    sticky_scrolls = block_scrolls();
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
    ui_esc(UI_CURSOR_SHOW);

}
