#include "block.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tty.h"
#include "ui.h"

// One more than any block worth painting; a block is capped at the screen.
#define ROWS_MAX 128

static int      top;
static int      rows;
static int      caret_at;
static int      caret_col_at;
static int      widths[ROWS_MAX];

static int      out_row = 1;
static int      out_col;
static int      anchored;
static unsigned scrolls;

static unsigned seen_epoch;
static int      seen_rows;
static int      seen_cols;

static int      row_edit = -1;

static void cup(int row, int col)
{
    char esc[32];
    snprintf(esc, sizeof esc, "\x1b[%d;%dH", row, col);
    ui_esc(esc);
}

static void erase_from(int row)
{
    cup(row, 1);
    ui_esc("\x1b[J");
}

static int moved(void)
{
    return !anchored || tty_resize_epoch() != seen_epoch || tty_rows() != seen_rows ||
           tty_screen_columns() != seen_cols;
}

// The terminal has reflowed under us, so nothing about the old block's position
// is known. Ask where the cursor is, work back to the rows the block occupies
// now, and wipe from there down; what the estimate misses the erase-below
// covers, and the next paint starts from an anchor the terminal agrees with.
static void resync(void)
{
    int H = tty_rows(), W = tty_screen_columns();
    int r = 0, c = 1;
    int have = tty_cursor_pos(&r, &c);

    if (rows > 0) {
        // The cursor was left on the caret, and a narrower screen wraps the row
        // it sits on: count the rows the caret's own row spends before it, or a
        // block whose first row wrapped is read as starting one row too low.
        int ref_row = caret_col_at >= 0 ? caret_at : rows - 1;
        int ref_col = caret_col_at >= 0 ? caret_col_at
                                        : (widths[rows - 1] > 0 ? widths[rows - 1] - 1 : 0);
        int above = ui_reflow_rows(widths, ref_row, W) + (W > 0 ? ref_col / W : 0);
        int all = ui_reflow_rows(widths, rows, W);

        // No answer: erase from whichever candidate is higher up. Wiping a row
        // of transcript that the reflow has already scrambled costs a repaint;
        // guessing too low leaves a copy of the block nothing will ever erase.
        int block_top = r - above;
        if (!have) {
            block_top = H - all + 1;
            if (top > 0 && top < block_top)
                block_top = top;
        }
        if (block_top < 1)
            block_top = 1;
        if (block_top > H)
            block_top = H;
        erase_from(block_top);
        out_row = block_top - (out_col ? 1 : 0);
        if (out_row < 1) {
            out_row = 1;
            out_col = 0;
        }
    } else if (have) {
        out_row = r;
        out_col = c - 1;
    } else if (!anchored) {
        out_row = H;
        out_col = 0;
    }

    if (out_row > H)
        out_row = H;
    if (W > 0 && out_col >= W)
        out_col = W - 1;

    rows = 0;
    top = 0;
    anchored = 1;
    seen_epoch = tty_resize_epoch();
    seen_rows = H;
    seen_cols = W;
}

static void sync_now(void)
{
    if (moved())
        resync();
}

void block_clear(void)
{
    sync_now();
    if (rows > 0)
        erase_from(top);
    rows = 0;
    top = 0;
    cup(out_row, out_col + 1);
    ui_flush();
}

// The rows named here stop being chrome and become transcript: the block gives
// them up, the rest of it is wiped, and the anchor moves below them.
void block_keep(int keep)
{
    sync_now();
    if (rows <= 0)
        return;
    if (keep < 0)
        keep = 0;
    if (keep > rows)
        keep = rows;
    out_row = top + keep;
    out_col = 0;
    if (out_row > tty_rows())
        out_row = tty_rows();
    erase_from(out_row);
    rows = 0;
    top = 0;
    cup(out_row, 1);
    ui_flush();
}

void block_forget(void)
{
    rows = 0;
    top = 0;
    anchored = 0;
}

void block_cleared(void)
{
    rows = 0;
    top = 0;
    out_row = 1;
    out_col = 0;
    anchored = 1;
    seen_epoch = tty_resize_epoch();
    seen_rows = tty_rows();
    seen_cols = tty_screen_columns();
}

int block_out_row(void) { return out_row; }

unsigned block_scrolls(void) { return scrolls; }

// The transcript is about to write where the block sits. This is the one place
// the rule is enforced, so no caller has to remember it: the block comes off
// screen here or it never turns into scrollback at all.
void block_before_output(void)
{
    if (rows > 0)
        block_clear();
}

static void feed_row(void)
{
    if (out_row < tty_rows())
        out_row++;
    else
        scrolls++;
}

void block_newline(void)
{
    out_col = 0;
    feed_row();
}

void block_wrote(const char *s, size_t n)
{
    int cols = tty_screen_columns();
    if (cols < 1)
        return;
    out_col += (int)ui_cells_n(s, n);
    while (out_col >= cols) {
        out_col -= cols;
        feed_row();
    }
}

void block_begin(void)
{
    sync_now();
    row_edit = -1;
    ui_sink_begin();
}

int block_have(void) { return rows > 0 && !moved(); }

void block_row_begin(int row)
{
    row_edit = row;
    ui_sink_begin();
}

static int split(char *body, char **line, size_t *len, int max)
{
    int n = 0;
    char *s = body;
    while (n < max) {
        char *nl = strchr(s, '\n');
        size_t l = nl ? (size_t)(nl - s) : strlen(s);
        while (l && s[l - 1] == '\r')
            l--;
        line[n] = s;
        len[n] = l;
        n++;
        if (!nl || !nl[1])
            break;
        s = nl + 1;
    }
    return n;
}

static void put_row(int at, const char *s, size_t n, int index)
{
    cup(at, 1);
    ui_esc("\x1b[K");
    ui_putn(s, n);
    if (index >= 0 && index < ROWS_MAX)
        widths[index] = (int)ui_cells_visible(s, n);
}

void block_end(int caret_row, int caret_col)
{
    char *body = ui_sink_end();
    if (!body) {
        block_clear();
        return;
    }

    char  *line[ROWS_MAX];
    size_t len[ROWS_MAX];
    int    n = split(body, line, len, ROWS_MAX);

    int H = tty_rows(), W = tty_screen_columns();
    if (n > H - 1)
        n = H - 1;
    if (n < 1) {
        free(body);
        block_clear();
        return;
    }

    ui_scroll_track(0);
    ui_sync_begin();
    ui_esc(UI_CURSOR_HIDE);

    int want = out_col ? out_row + 1 : out_row;
    if (want < 1)
        want = 1;

    // The block would run past the last row, so give it room the way printing
    // would: scroll the transcript up and follow it with the anchor.
    int over = (want + n - 1) - H;
    if (over > 0) {
        cup(H, 1);
        for (int i = 0; i < over; i++)
            ui_esc("\n");
        want -= over;
        out_row -= over;
        scrolls += (unsigned)over;
        if (out_row < 1)
            out_row = 1;
    }

    if (top > 0 && top < want)
        erase_from(top);

    ui_esc("\x1b[?7l");
    for (int i = 0; i < n; i++)
        put_row(want + i, line[i], len[i], i);
    ui_esc("\x1b[?7h");

    if (want + n <= H)
        erase_from(want + n);

    top = want;
    rows = n;
    caret_at = caret_row >= 0 && caret_row < n ? caret_row : n - 1;
    caret_col_at = caret_col;

    if (caret_col >= 0) {
        int col = caret_col + 1;
        if (col > W)
            col = W;
        cup(top + caret_at, col);
        ui_esc(UI_CURSOR_SHOW);
    }

    ui_sync_end();
    ui_scroll_track(1);
    ui_flush();
    free(body);
}

void block_row_end(void)
{
    char *body = ui_sink_end();
    int   at = row_edit;
    row_edit = -1;
    if (!body)
        return;
    if (at < 0 || at >= rows) {
        free(body);
        return;
    }

    char  *line[ROWS_MAX];
    size_t len[ROWS_MAX];
    int    n = split(body, line, len, ROWS_MAX);

    ui_scroll_track(0);
    ui_sync_begin();
    ui_esc(UI_CURSOR_HIDE);
    ui_esc("\x1b[?7l");
    for (int i = 0; i < n && at + i < rows; i++)
        put_row(top + at + i, line[i], len[i], at + i);
    ui_esc("\x1b[?7h");

    if (caret_col_at >= 0) {
        int W = tty_screen_columns();
        int col = caret_col_at + 1;
        if (col > W)
            col = W;
        cup(top + caret_at, col);
        ui_esc(UI_CURSOR_SHOW);
    }
    ui_sync_end();
    ui_scroll_track(1);
    ui_flush();
    free(body);
}
