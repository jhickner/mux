// A model of the screen the block paints onto: it applies the bytes mux writes
// and keeps both the visible rows and what scrolled off, so the one rule the
// block lives by — a painted block is never on screen when the transcript
// writes, and never survives into the scrollback — can be asserted instead of
// eyeballed in a real terminal.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "block.h"
#include "tty.h"
#include "ui.h"

#define ROWS_MAX 64
#define COLS_MAX 256
#define HIST_MAX 512

struct screen {
    int  rows, cols;
    char cell[ROWS_MAX][COLS_MAX + 1];
    char hist[HIST_MAX][COLS_MAX + 1];
    int  hist_n;
    int  cur_r, cur_c;
    int  wrap;
};

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

static void screen_init(struct screen *s, int rows, int cols)
{
    memset(s, 0, sizeof *s);
    s->rows = rows;
    s->cols = cols;
    s->wrap = 1;
    for (int r = 0; r < rows; r++)
        memset(s->cell[r], ' ', (size_t)cols);
}

static void screen_scroll(struct screen *s)
{
    if (s->hist_n < HIST_MAX)
        memcpy(s->hist[s->hist_n++], s->cell[0], (size_t)s->cols);
    for (int r = 1; r < s->rows; r++)
        memcpy(s->cell[r - 1], s->cell[r], (size_t)s->cols);
    memset(s->cell[s->rows - 1], ' ', (size_t)s->cols);
}

static void screen_linefeed(struct screen *s)
{
    if (s->cur_r + 1 < s->rows)
        s->cur_r++;
    else
        screen_scroll(s);
}

static void screen_putc(struct screen *s, char c)
{
    if (s->cur_c >= s->cols) {
        if (!s->wrap) {
            s->cell[s->cur_r][s->cols - 1] = c;
            return;
        }
        s->cur_c = 0;
        screen_linefeed(s);
    }
    s->cell[s->cur_r][s->cur_c++] = c;
}

static void erase_eol(struct screen *s)
{
    for (int c = s->cur_c; c < s->cols; c++)
        s->cell[s->cur_r][c] = ' ';
}

static void erase_below(struct screen *s)
{
    erase_eol(s);
    for (int r = s->cur_r + 1; r < s->rows; r++)
        memset(s->cell[r], ' ', (size_t)s->cols);
}

// Only the sequences the block and the transcript actually emit; anything else
// would be a change this model has to learn about before the test can be
// trusted, so it counts as a failure rather than being skipped quietly.
static size_t apply_csi(struct screen *s, const char *p, size_t n)
{
    size_t i = 0;
    char   priv = 0;
    if (i < n && (p[i] == '?' || p[i] == '>')) {
        priv = p[i];
        i++;
    }
    int args[4] = {0, 0, 0, 0};
    int argc = 0;
    while (i < n) {
        if (p[i] >= '0' && p[i] <= '9') {
            if (argc == 0)
                argc = 1;
            if (argc <= 4)
                args[argc - 1] = args[argc - 1] * 10 + (p[i] - '0');
            i++;
            continue;
        }
        if (p[i] == ';') {
            argc = argc < 4 ? argc + 1 : argc;
            i++;
            continue;
        }
        break;
    }
    if (i >= n)
        return i;

    char final = p[i++];
    if (priv) {
        if (final == 'h' || final == 'l') {
            if (args[0] == 7)
                s->wrap = final == 'h';
        } else {
            fail("unmodelled private sequence");
        }
        return i;
    }

    switch (final) {
    case 'H': {
        int r = argc > 0 && args[0] > 0 ? args[0] - 1 : 0;
        int c = argc > 1 && args[1] > 0 ? args[1] - 1 : 0;
        s->cur_r = r < s->rows ? r : s->rows - 1;
        s->cur_c = c < s->cols ? c : s->cols - 1;
        break;
    }
    case 'K':
        erase_eol(s);
        break;
    case 'J':
        if (args[0] == 2) {
            for (int r = 0; r < s->rows; r++)
                memset(s->cell[r], ' ', (size_t)s->cols);
        } else {
            erase_below(s);
        }
        break;
    case 'A':
        s->cur_r -= args[0] > 0 ? args[0] : 1;
        if (s->cur_r < 0)
            s->cur_r = 0;
        break;
    case 'm':
        break;
    default:
        fail("unmodelled control sequence");
        break;
    }
    return i;
}

static void screen_feed(struct screen *s, const char *p, size_t n)
{
    for (size_t i = 0; i < n;) {
        char c = p[i];
        if (c == 0x1b) {
            if (i + 1 < n && p[i + 1] == '[') {
                i += 2 + apply_csi(s, p + i + 2, n - i - 2);
                continue;
            }
            if (i + 1 < n && p[i + 1] == ']') {
                i += 2;
                while (i < n && p[i] != 0x07)
                    i++;
                i += i < n;
                continue;
            }
            i += 2;
            continue;
        }
        if (c == '\n') {
            screen_linefeed(s);
            i++;
            continue;
        }
        if (c == '\r') {
            s->cur_c = 0;
            i++;
            continue;
        }
        screen_putc(s, c);
        i++;
    }
}

// Everything mux has written since the last look, handed to the model. The
// read end is its own descriptor so following the stream cannot disturb where
// stdout is writing.
static int tap_read = -1;

static void pump(struct screen *s)
{
    fflush(stdout);
    char    buf[8192];
    ssize_t n;
    while ((n = read(tap_read, buf, sizeof buf)) > 0)
        screen_feed(s, buf, (size_t)n);
}

static void drop(void)
{
    fflush(stdout);
    char    buf[8192];
    while (read(tap_read, buf, sizeof buf) > 0)
        ;
}

static int on_screen(const struct screen *s, const char *needle)
{
    for (int r = 0; r < s->rows; r++) {
        char row[COLS_MAX + 1];
        memcpy(row, s->cell[r], (size_t)s->cols);
        row[s->cols] = '\0';
        if (strstr(row, needle))
            return 1;
    }
    return 0;
}

static int in_history(const struct screen *s, const char *needle)
{
    for (int r = 0; r < s->hist_n; r++) {
        char row[COLS_MAX + 1];
        memcpy(row, s->hist[r], (size_t)s->cols);
        row[s->cols] = '\0';
        if (strstr(row, needle))
            return 1;
    }
    return 0;
}

static int anywhere(const struct screen *s, const char *needle)
{
    return on_screen(s, needle) || in_history(s, needle);
}

static void paint(int rows, int caret_row, int caret_col, const char *tag)
{
    block_begin();
    for (int i = 0; i < rows; i++) {
        if (i)
            ui_put("\n");
        ui_printf("%s-row%d", tag, i);
    }
    block_end(caret_row, caret_col);
}

static void say(const char *text)
{
    ui_put(text);
    ui_put("\n");
    ui_flush();
}

// The block's anchor is a claim about the terminal: the row it says the
// transcript writes to has to be the row the terminal is actually on.
static void check_anchor(const struct screen *s, const char *what)
{
    if (block_out_row() != s->cur_r + 1)
        fail(what);
}

static void check_write_over_block(struct screen *s)
{
    say("transcript one");
    paint(3, 2, 4, "BLOCK");
    pump(s);
    if (!on_screen(s, "BLOCK-row0") || !on_screen(s, "BLOCK-row2"))
        fail("the block is painted");

    // No erase first: the rule has to hold whether or not the caller remembers.
    say("transcript two");
    pump(s);
    if (anywhere(s, "BLOCK-row0") || anywhere(s, "BLOCK-row2"))
        fail("a write past the block leaves no copy of it");
    if (!on_screen(s, "transcript two"))
        fail("the write itself lands on screen");
    check_anchor(s, "the anchor follows a write that displaced the block");
}

static void check_clear(struct screen *s)
{
    paint(2, 1, 2, "GONE");
    pump(s);
    block_clear();
    pump(s);
    if (anywhere(s, "GONE-row0") || anywhere(s, "GONE-row1"))
        fail("clearing takes the whole block off screen");
    check_anchor(s, "the anchor is where the block was cleared to");
}

static void check_repaint_shrinks(struct screen *s)
{
    paint(4, 0, 0, "TALL");
    pump(s);
    paint(1, 0, 0, "SHORT");
    pump(s);
    if (anywhere(s, "TALL-row3"))
        fail("a shorter block wipes the rows the taller one had");
    if (!on_screen(s, "SHORT-row0"))
        fail("the shorter block is painted");
    block_clear();
    pump(s);
}

static void check_bottom_scrolls(struct screen *s)
{
    for (int i = 0; i < 40; i++) {
        char line[64];
        snprintf(line, sizeof line, "filler %d", i);
        say(line);
    }
    pump(s);
    if (block_out_row() != s->rows)
        fail("the anchor sits at the last row once the screen is full");

    int was = s->hist_n;
    paint(3, 2, 0, "PUSH");
    pump(s);
    if (!on_screen(s, "PUSH-row0") || !on_screen(s, "PUSH-row2"))
        fail("a block at the bottom is painted in full");
    if (s->hist_n != was + 2)
        fail("making room for it scrolls the transcript up by the rows it needs");
    if (!on_screen(s, "filler 39"))
        fail("the transcript it scrolled past is still the row above it");

    say("after the push");
    pump(s);
    if (anywhere(s, "PUSH-row0"))
        fail("the pushed block leaves no copy behind");
    check_anchor(s, "the anchor survives a block that had to scroll");
}

static void check_taller_than_screen(struct screen *s)
{
    paint(tty_rows() + 4, 0, 0, "HUGE");
    pump(s);
    block_clear();
    pump(s);
    if (anywhere(s, "HUGE-row0"))
        fail("a block taller than the screen is clipped to what can be erased");
    check_anchor(s, "the anchor is sane after an oversized block");
}

// A resize with no cursor report to fall back on: the terminal is not a tty
// here, so this is the blind path, and it still may not strand a block.
static void check_resize(struct screen *s, int cols, int rows)
{
    paint(3, 1, 2, "RESIZED");
    pump(s);

    char buf[16];
    snprintf(buf, sizeof buf, "%d", cols);
    setenv("COLUMNS", buf, 1);
    snprintf(buf, sizeof buf, "%d", rows);
    setenv("LINES", buf, 1);
    screen_init(s, rows, cols);
    drop();

    say("after the resize");
    pump(s);
    if (anywhere(s, "RESIZED-row0"))
        fail("a resized-away block leaves no copy on the new screen");
    check_anchor(s, "the anchor is re-established after a resize");
}

int main(void)
{
    setenv("COLUMNS", "80", 1);
    setenv("LINES", "24", 1);

    char path[] = "/tmp/" APP_NAME "-blocktest-XXXXXX";
    int  wfd = mkstemp(path);
    tap_read = wfd >= 0 ? open(path, O_RDONLY) : -1;
    if (wfd < 0 || tap_read < 0) {
        fprintf(stderr, "blocktest: no temp file\n");
        return 1;
    }
    unlink(path);
    fflush(stdout);
    if (dup2(wfd, STDOUT_FILENO) < 0) {
        fprintf(stderr, "blocktest: cannot redirect stdout\n");
        return 1;
    }
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);

    ui_init();
    ui_raw(1);

    struct screen s;
    screen_init(&s, 24, 80);

    check_write_over_block(&s);
    check_clear(&s);
    check_repaint_shrinks(&s);
    check_bottom_scrolls(&s);
    check_taller_than_screen(&s);
    check_resize(&s, 40, 24);
    check_resize(&s, 100, 12);

    fflush(stdout);
    if (failures)
        return 1;
    fprintf(stderr, "blocktest: all checks passed\n");
    return 0;
}
