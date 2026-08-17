#include "app.h"
#include "status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

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
static status_offset_fn below_offset;
static void            *below_ud;
static status_above_fn  above;
static status_offset_fn above_rows_fn;
static void            *above_ud;
static int              above_painted;
static int              caret_row;
static int              caret_col;
static int              painted;
static int              spin_width;
static int              gap;
static int              painted_gap;
static int              dirty;

#define STICKY_ROWS_MAX 16
static int   sticky_on;
static char *sticky_text;
static int   sticky_widths[STICKY_ROWS_MAX];
static int   sticky_rows;
static int   sticky_drawn;
static int   sticky_tracking;
static int   block_tallest;

static int   chrome_rows = 4;

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

void status_set_below(status_paint_fn paint_fn, status_offset_fn offset_fn, void *ud)
{
    below = paint_fn;
    below_offset = offset_fn;
    below_ud = ud;
}

void status_set_above(status_above_fn paint_fn, status_offset_fn rows_fn, void *ud)
{
    above = paint_fn;
    above_rows_fn = rows_fn;
    above_ud = ud;
}

static int above_rows(void)
{
    return above_painted && above_rows_fn ? above_rows_fn(above_ud) : 0;
}

static int size_changing(void)
{
    unsigned epoch = tty_resize_epoch();
    if (epoch != resize_epoch) {
        resize_epoch = epoch;
        resize_at = now_seconds();
        dirty = 1;

        block_tallest = 0;
    }
    return resize_at > 0 && (now_seconds() - resize_at) * 1000.0 < TTY_RESIZE_SETTLE_MS;
}

static int rows_above_caret(void)
{
    int cols = ui_columns();
    int spin = spin_width ? ui_reflow_rows(&spin_width, 1, cols) : 0;
    int head = sticky_rows ? ui_reflow_rows(sticky_widths, sticky_rows, cols) : 0;

    if (!below)
        return painted_gap + head + above_rows() + (spin > 0 ? spin - 1 : 0);
    return painted_gap + head + above_rows() + spin +
           (below_offset ? below_offset(below_ud) : caret_row - 1);
}

static void erase_block(void)
{
    ui_esc(UI_CURSOR_HIDE);
    if (painted)
        ui_move(rows_above_caret(), 'A');
    ui_esc("\r\x1b[J");
    caret_row = 0;
    spin_width = 0;
    painted_gap = 0;
    sticky_rows = 0;
    above_painted = 0;
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

static int sticky_gone(void)
{
    if (!sticky_tracking)
        return 1;
    int gone_at = tty_rows() - 2 - (block_tallest > 0 ? block_tallest - 1 : 0);
    return ui_scroll_rows() >= (gone_at > 0 ? gone_at : 0);
}

static void paint_sticky(void)
{
    sticky_drawn = 0;
    if (!sticky_on || !sticky_text || !*sticky_text || !sticky_gone())
        return;

    int cols = ui_columns();
    int max = STICKY_LINES;
    if (max > STICKY_ROWS_MAX - 1)
        max = STICKY_ROWS_MAX - 1;

    struct ui_wrap w = {0};
    w.budget = (size_t)(cols - 3 > 4 ? cols - 3 : 4);
    w.gutter = UI_BAR " ";
    w.role = UI_STICKY;
    w.max_rows = max;
    w.erase = 1;
    w.widths = sticky_widths;
    w.widths_max = STICKY_ROWS_MAX - 1;

    sticky_rows = ui_wrap_paint(sticky_text, &w);
    if (sticky_rows > w.widths_max)
        sticky_rows = w.widths_max;

    sticky_drawn = sticky_rows;

    ui_esc(UI_ERASE_EOL);
    ui_put("\n");
    sticky_widths[sticky_rows++] = 0;
}

int status_sticky_rows(void)
{
    return sticky_drawn;
}

static void block_rows(int below_rows)
{
    int cols = ui_columns();
    chrome_rows = painted_gap + below_rows + above_rows() +
                  (sticky_rows ? ui_reflow_rows(sticky_widths, sticky_rows, cols) : 0) +
                  (spin_width ? ui_reflow_rows(&spin_width, 1, cols) : 0);
    if (chrome_rows > block_tallest)
        block_tallest = chrome_rows;
}

int status_chrome_rows(void) { return chrome_rows; }

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
    ui_sync_begin();

    ui_scroll_track(0);
    erase_block();

    painted_gap = gap;
    if (painted_gap)
        ui_put("\n");
    paint_sticky();

    if (above) {
        above(above_ud);
        above_painted = 1;
    }

    paint_spin();

    int below_rows = 0;
    if (below) {
        int rows = 1, row = 0, col = 0;
        ui_put("\n");
        below(below_ud, &rows, &row, &col);
        ui_move((rows - 1) - row, 'A');
        ui_esc("\r");
        ui_move(col, 'C');
        caret_row = 1 + row;
        caret_col = col;
        below_rows = rows;
        ui_esc(UI_CURSOR_SHOW);
    }
    block_rows(below_rows);
    painted = 1;
    dirty = 0;
    ui_scroll_track(1);
    ui_sync_end();
    ui_flush();
}

static int paint_spin_only(void)
{
    int cols = ui_columns();
    if (!painted || !below || cols < 24)
        return 0;

    if (!spin_width || ui_reflow_rows(&spin_width, 1, cols) != 1)
        return 0;

    int down = 1 + (below_offset ? below_offset(below_ud) : caret_row - 1);
    ui_sync_begin();
    ui_scroll_track(0);
    ui_esc(UI_CURSOR_HIDE);
    ui_move(down, 'A');
    ui_esc("\r\x1b[K");
    paint_spin();
    ui_esc("\r");
    ui_move(down, 'B');
    ui_move(caret_col, 'C');
    ui_esc(UI_CURSOR_SHOW);
    ui_scroll_track(1);
    ui_sync_end();
    ui_flush();
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
    caret_row = 0;
    spin_width = 0;
    gap = 0;
    painted_gap = 0;
    sticky_rows = 0;
    block_tallest = 0;
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
    ui_sync_begin();
    erase_block();
    ui_sync_end();
    ui_flush();
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
    ui_scroll_mark();
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
    if (visible) {
        ui_sync_begin();
        erase_block();
        ui_sync_end();
    }
    visible = 0;
    active = 0;
    started = 0;
    ui_esc(UI_CURSOR_SHOW);

}
