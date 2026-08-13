#include "status.h"

#include <stdio.h>
#include <sys/time.h>

#include "tty.h"
#include "ui.h"

static const char *const FRAMES[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
#define FRAME_COUNT ((int)(sizeof FRAMES / sizeof *FRAMES))

/* The spinner advances on its own clock so the animation stays smooth however
 * often the caller happens to tick. */
#define FRAME_MS 90

/* What the turn is doing, in one word. Naming the tool and its argument here
 * put whole shell pipelines on the row; the row is a heartbeat, not a log. */
#define SPIN_WORD " · thinking"

static double  started;
static int     visible;
static int     frame;
static double  frame_at;

static status_paint_fn  below;
static status_offset_fn below_offset;
static void            *below_ud;
static int              caret_row;  /* rows from the spinner row down to the caret */
static int              painted;    /* a block is on screen */
static int              spin_width; /* cells the spinner row occupies */

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
 * reaches the spinner row's last. */
static int rows_above_caret(void)
{
    int cols = ui_columns();
    int spin = spin_width ? ui_reflow_rows(&spin_width, 1, cols) : 0;
    if (!below)
        return spin > 0 ? spin - 1 : 0;
    return spin + (below_offset ? below_offset(below_ud) : caret_row - 1);
}

/* Wipe the spinner row and everything the block painted below it, leaving the
 * cursor at the start of the spinner row. Emits without flushing so a caller can
 * bracket the erase and the redraw that follows into one frame. */
static void erase_block(void)
{
    ui_esc("\x1b[?25l");
    if (painted)
        move(rows_above_caret(), 'A');
    ui_esc("\r\x1b[J");
    caret_row = 0;
    spin_width = 0;
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
    ui_esc(ui_style(UI_ACCENT));
    ui_put(left);
    spin_width = (int)ui_cells(left);
    /* Keep a column clear at the margin: a row filled to the edge leaves the
     * terminal holding a deferred wrap, which resolves into a second row that
     * the next resize then rewraps. */
    int word = (int)ui_cells(SPIN_WORD);
    if (spin_width + word <= ui_columns() - 1) {
        ui_esc(ui_style(UI_DIM));
        ui_put(SPIN_WORD);
        spin_width += word;
    }
    ui_esc(ui_style(UI_RESET));

    if (below) {
        int rows = 1, row = 0, col = 0;
        ui_esc("\r\n");
        below(below_ud, &rows, &row, &col);
        move((rows - 1) - row, 'A');
        ui_esc("\r");
        move(col, 'C');
        caret_row = 1 + row;
        ui_esc("\x1b[?25h"); /* the caret marks where typing lands */
    }
    painted = 1;
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
    painted = 0;
    paint();
}

void status_tick(void)
{
    if (!visible || size_changing())
        return;
    double t = now_seconds();
    if ((t - frame_at) * 1000.0 >= FRAME_MS) {
        frame = (frame + 1) % FRAME_COUNT;
        frame_at = t;
    }
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
