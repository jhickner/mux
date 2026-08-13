#include "status.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "tty.h"
#include "ui.h"

static const char *const FRAMES[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
#define FRAME_COUNT ((int)(sizeof FRAMES / sizeof *FRAMES))

/* The spinner advances on its own clock so the animation stays smooth however
 * often the caller happens to tick. */
#define FRAME_MS 90

static double  started;
static int     visible;
static int     frame;
static double  frame_at;
static char    word[64] = "working"; /* what the turn is doing */
static char    note[128]; /* the conversation's name, when it has one */

static status_paint_fn  below;
static status_offset_fn below_offset;
static void            *below_ud;
static int              caret_row;  /* rows from the spinner row down to the caret */
static int              painted;    /* a block is on screen */
static int              spin_width; /* cells the spinner row occupies */
static int              gap;        /* asked for a blank row above the spinner */
static int              painted_gap;/* ... and whether the block on screen has one */
static int              dirty;      /* the block on screen is out of date */

/* Set when a resize arrives, cleared once the size has been quiet again. */
static unsigned resize_epoch;
static double   resize_at;

static double now_seconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

double status_elapsed(void) { return now_seconds() - started; }

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

/* True while the window is still being dragged or zoomed.
 *
 * The erase walks up from the cursor by rows measured against the width read
 * back from the terminal, which is only right when that width and the rows
 * on screen agree. Mid-drag they need not: the multiplexer rewraps its grid and
 * resizes the pty as separate steps, so a paint landing between the two climbs
 * by a count for one layout while the other is on screen, and leaves the rows it
 * failed to reach behind. Sitting out the burst keeps paints on the settled side
 * of that split. */
static int size_changing(void)
{
    unsigned epoch = tty_resize_epoch();
    if (epoch != resize_epoch) {
        resize_epoch = epoch;
        resize_at = now_seconds();
    }
    return resize_at > 0 && (now_seconds() - resize_at) * 1000.0 < TTY_RESIZE_SETTLE_MS;
}

static void move(int count, char direction)
{
    if (count <= 0)
        return;
    char esc[16];
    snprintf(esc, sizeof esc, "\x1b[%d%c", count, direction);
    ui_esc(esc);
}

/* Rows from the cursor up to the first row of the block, as the terminal shows
 * them now. A resize rewraps what is on screen, so every row that contributes is
 * re-measured against the current width instead of the width it was drawn at;
 * walking up by the count from that paint would leave its top rows stranded.
 *
 * Without a block below, the cursor rests on the spinner row's last physical
 * row. With one, the offset lands on the block's first row and one more step
 * reaches the spinner row's last. The leading blank row counts as painted, so
 * the walk is measured against what is on screen rather than what is now
 * asked for. */
static int rows_above_caret(void)
{
    int cols = ui_columns();
    int spin = spin_width ? ui_reflow_rows(&spin_width, 1, cols) : 0;
    if (!below)
        return painted_gap + (spin > 0 ? spin - 1 : 0);
    return painted_gap + spin + (below_offset ? below_offset(below_ud) : caret_row - 1);
}

/* Wipe the spinner row and everything the block painted below it, leaving the
 * cursor at the start of the block. Emits without flushing so a caller can
 * bracket the erase and the redraw that follows into one frame. */
static void erase_block(void)
{
    ui_esc("\x1b[?25l");
    if (painted)
        move(rows_above_caret(), 'A');
    ui_esc("\r\x1b[J");
    caret_row = 0;
    spin_width = 0;
    painted_gap = 0;
    painted = 0;
}

/* "12s" up to a minute, then "1m 3s", then "1h 4m": a bare second count stops
 * being readable as a duration once a turn runs long. */
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

static void paint(void)
{
    char clock[32], left[64];
    humanize(status_elapsed(), clock, sizeof clock);
    snprintf(left, sizeof left, "%s %s", FRAMES[frame], clock);

    ui_sync_begin();
    erase_block();
    /* Part of the block, not of scrollback: it is erased along with the spinner,
     * so the permanent spacing between tool rows stays the caller's business. */
    painted_gap = gap;
    if (painted_gap)
        ui_put("\n");
    ui_esc(ui_style(UI_ACCENT));
    ui_put(left);
    spin_width = (int)ui_cells(left);
    /* Keep a column clear at the margin: a row filled to the edge leaves the
     * terminal holding a deferred wrap, which resolves into a second row that
     * the next resize then rewraps. */
    char spin_word[80];
    snprintf(spin_word, sizeof spin_word, " · %s", word[0] ? word : "working");
    int word_width = (int)ui_cells(spin_word);
    if (spin_width + word_width <= ui_columns() - 1) {
        ui_esc(ui_style(UI_DIM));
        ui_put(spin_word);
        spin_width += word_width;
    }
    /* The name is the least important thing on the row: it goes last, and only
     * when the row has room for all of it. */
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

    if (below) {
        int rows = 1, row = 0, col = 0;
        ui_put("\n");
        below(below_ud, &rows, &row, &col);
        move((rows - 1) - row, 'A');
        ui_esc("\r");
        move(col, 'C');
        caret_row = 1 + row;
        ui_esc("\x1b[?25h"); /* the caret marks where typing lands */
    }
    painted = 1;
    dirty = 0;
    ui_sync_end();
    ui_flush();
}

void status_begin(void)
{
    started = now_seconds();
    frame = 0;
    frame_at = started;
    visible = 1;
    caret_row = 0;
    spin_width = 0;
    gap = 0;
    painted_gap = 0;
    painted = 0;
    paint();
}

/* Called as often as the driver polls its abort predicate — which is far more
 * often than the animation moves, now that a keystroke has to reach the screen
 * within a tick. Painting each time would push a full erase-and-redraw of the
 * block at the terminal dozens of times a second, so a tick that has nothing
 * new to show does nothing. */
void status_tick(void)
{
    if (!visible || size_changing())
        return;
    double t = now_seconds();
    if ((t - frame_at) * 1000.0 >= FRAME_MS) {
        frame = (frame + 1) % FRAME_COUNT;
        frame_at = t;
        dirty = 1;
    }
    if (dirty)
        paint();
}

void status_pause(void)
{
    if (!visible)
        return;
    ui_sync_begin();
    erase_block();
    ui_sync_end();
    ui_flush();
    visible = 0;
}

void status_resume(void)
{
    if (visible)
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
    if (visible && !size_changing())
        paint();
}

void status_end(void)
{
    if (visible) {
        ui_sync_begin();
        erase_block();
        ui_sync_end();
    }
    visible = 0;
    ui_esc("\x1b[?25h");
    ui_flush();
}
