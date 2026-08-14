#include "status.h"

#include <stdio.h>
#include <stdlib.h>
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
static int     active;  /* status_begin() has run and status_end() has not */
static int     visible;
static int     frame;
static double  frame_at;
static char    word[64] = "working"; /* what the turn is doing */
static char    note[128]; /* the conversation's name, when it has one */

static status_paint_fn  below;
static status_offset_fn below_offset;
static void            *below_ud;
static status_hud_fn    hud;
static void            *hud_ud;
static int              hud_rows;   /* chrome rows in the block on screen */
static int              caret_row;  /* rows from the spinner row down to the caret */
static int              caret_col;  /* and the column it sat at */
static int              painted;    /* a block is on screen */
static int              spin_width; /* cells the spinner row occupies */
static int              gap;        /* asked for a blank row above the spinner */
static int              painted_gap;/* ... and whether the block on screen has one */
static int              dirty;      /* the block on screen is out of date */

/* The floating prompt above the spinner. Its rows are remembered by the cells
 * they occupy, not by their count: a resize rewraps them on screen, and the
 * erase has to walk up by what is there now. */
#define STICKY_ROWS_MAX 16
static int   sticky_on;
static char *sticky_text;
static int   sticky_widths[STICKY_ROWS_MAX];
static int   sticky_rows;   /* rows of prompt in the block on screen, with its
                             * trailing blank; 0 when none is painted */
static int   sticky_screen;  /* rows the screen had when the message was echoed */
static int   block_tallest;  /* the tallest the block has been this turn */

/* Set when a resize arrives, cleared once the size has been quiet again. */
static unsigned resize_epoch;
static double   resize_at;

static double now_seconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

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

void status_set_hud(status_hud_fn paint_fn, void *ud)
{
    hud = paint_fn;
    hud_ud = ud;
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
    int head = sticky_rows ? ui_reflow_rows(sticky_widths, sticky_rows, cols) : 0;
    /* The chrome rows are clipped to the width, so a resize cannot rewrap
     * them: the count they were painted at still holds. */
    head += hud_rows;
    if (!below)
        return painted_gap + head + (spin > 0 ? spin - 1 : 0);
    return painted_gap + head + spin +
           (below_offset ? below_offset(below_ud) : caret_row - 1);
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
    sticky_rows = 0;
    hud_rows = 0;
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

/* Whether output has carried the echoed message off the top of the screen.
 *
 * The echo's own blank row and the row the cursor was left on are the two that
 * never have to scroll; the block below holds the rest of the output that much
 * further from the bottom, so the count is judged against a bottom raised by
 * the tallest the block has been. Both terms only ever move one way within a
 * turn, so this cannot flip back once it is true. */
static int sticky_gone(void)
{
    int gone_at = sticky_screen - 2 - (block_tallest > 0 ? block_tallest - 1 : 0);
    return ui_scroll_rows() >= (gone_at > 0 ? gone_at : 0);
}

/* The prompt the turn is answering, drawn as the "▌" bar it wears in
 * scrollback and closed by a blank row. It waits until the echo it copies has
 * scrolled away, so the message is never on screen twice. Long messages are
 * clipped to STICKY_LINES so the block never crowds out the output it floats
 * over; the last row shown ends in an ellipsis. Leaves the cursor at the start
 * of the spinner row and records what it drew for the next erase. */
static void paint_sticky(void)
{
    if (!sticky_on || !sticky_text || !*sticky_text || !sticky_gone())
        return;

    int cols = ui_columns();
    size_t budget = (size_t)(cols - 3 > 4 ? cols - 3 : 4);
    int max = STICKY_LINES;
    if (max > STICKY_ROWS_MAX - 1)
        max = STICKY_ROWS_MAX - 1;

    const char *p = sticky_text;
    while (*p && sticky_rows < max) {
        size_t skip = 0;
        size_t row = ui_wrap_row(p, budget, &skip);
        int width = 2 + (int)ui_cells_n(p, row);
        ui_esc("\x1b[K");
        ui_esc(ui_style(UI_STICKY));
        ui_put(UI_BAR " ");
        ui_putn(p, row);
        p += row + skip;
        if (*p && sticky_rows + 1 == max) {
            ui_put("…");
            width++;
        }
        ui_esc(ui_style(UI_RESET));
        ui_put("\n");
        sticky_widths[sticky_rows++] = width;
    }
    ui_esc("\x1b[K");
    ui_put("\n");
    sticky_widths[sticky_rows++] = 0; /* the blank row closing the block */
}

/* Rows the block just painted covers, kept as the tallest of the turn.
 *
 * The block sits below the output, between it and the bottom of the screen, so
 * those rows are headroom the output never gets: the screen starts scrolling
 * that much sooner. What the block draws is left out of the scrollback count —
 * it is erased and redrawn in place — so the count has to be judged against a
 * bottom raised by this much. The tallest is the one that counts, since a block
 * that grew pushed the ceiling down and a later, shorter one does not lift it
 * back. */
static void block_rows(int below_rows)
{
    int cols = ui_columns();
    int rows = painted_gap + below_rows + hud_rows +
               (sticky_rows ? ui_reflow_rows(sticky_widths, sticky_rows, cols) : 0) +
               (spin_width ? ui_reflow_rows(&spin_width, 1, cols) : 0);
    if (rows > block_tallest)
        block_tallest = rows;
}

/* The spinner row itself, from column 0 of a row already cleared. Records the
 * cells it filled, which the erase walk measures against. */
static void paint_spin(void)
{
    char clock[32], left[64];
    humanize(status_elapsed(), clock, sizeof clock);
    snprintf(left, sizeof left, "%s %s", FRAMES[frame], clock);

    ui_esc(ui_style(UI_BRAND));
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
}

static void paint(void)
{
    ui_sync_begin();
    /* The block is redrawn in place, so what it writes scrolls nothing that
     * outlives it: it stays out of the scrollback count. */
    ui_scroll_track(0);
    erase_block();
    /* Part of the block, not of scrollback: it is erased along with the spinner,
     * so the permanent spacing between tool rows stays the caller's business. */
    painted_gap = gap;
    if (painted_gap)
        ui_put("\n");
    paint_sticky();
    /* The chrome the idle prompt carries above its caret, painted here instead
     * while a turn runs so it keeps its place above the spinner. */
    hud_rows = hud ? hud(hud_ud, ui_columns()) : 0;
    paint_spin();

    int below_rows = 0;
    if (below) {
        int rows = 1, row = 0, col = 0;
        ui_put("\n");
        below(below_ud, &rows, &row, &col);
        move((rows - 1) - row, 'A');
        ui_esc("\r");
        move(col, 'C');
        caret_row = 1 + row;
        caret_col = col;
        below_rows = rows;
        ui_esc("\x1b[?25h"); /* the caret marks where typing lands */
    }
    block_rows(below_rows);
    painted = 1;
    dirty = 0;
    ui_scroll_track(1);
    ui_sync_end();
    ui_flush();
}

/* A tick that only moves the spinner on rewrites the spinner's row where it
 * stands and puts the cursor back, rather than taking the block down and
 * drawing it again: the chrome above and the prompt below have not changed, and
 * redrawing them ten times a second is what shows up as flicker. Returns 0 when
 * the geometry is not certain enough to reach into, leaving the full paint to
 * the caller. */
static int paint_spin_only(void)
{
    int cols = ui_columns();
    if (!painted || !below || cols < 24)
        return 0;
    /* A wrapped spinner row would put the rows below it somewhere other than
     * where this counts on finding them. */
    if (!spin_width || ui_reflow_rows(&spin_width, 1, cols) != 1)
        return 0;

    int down = 1 + (below_offset ? below_offset(below_ud) : caret_row - 1);
    ui_sync_begin();
    ui_scroll_track(0);
    ui_esc("\x1b[?25l");
    move(down, 'A');
    ui_esc("\r\x1b[K");
    paint_spin();
    ui_esc("\r");
    move(down, 'B');
    move(caret_col, 'C');
    ui_esc("\x1b[?25h");
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
    hud_rows = 0;
    block_tallest = 0;
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
    if (active && visible && !size_changing())
        paint();
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

/* Called with the message already echoed and the cursor on the row below it.
 * However far down the screen that leaves it, the output needed to carry the
 * echo off the top comes to the same thing: the rows between the cursor and the
 * bottom, which scroll nothing, and then one row per row of the echo above it.
 * That is the height of the screen, less the echo's own blank row and the row
 * the cursor sits on. */
void status_sticky_prompt(const char *text)
{
    free(sticky_text);
    sticky_text = text && *text ? strdup(text) : NULL;
    dirty = 1;

    sticky_screen = tty_rows();
    ui_scroll_mark();
}

void status_sticky_erased(void) { sticky_screen = 0; }

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
    ui_esc("\x1b[?25h");
    /* Left unflushed on purpose: the idle prompt redraws the same rows with the
     * turn summary where the spinner was, and its flush carries this erase out
     * with it. Flushing here would show the gap in between as a blink. */
}
