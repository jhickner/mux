#ifndef SCREENMODEL_H
#define SCREENMODEL_H

// A model of the screen the viewport paints onto, applying only the sequences
// it emits. The painter sends just what changed, so a test that wants to know
// what is on screen has to keep the screen the way a terminal does rather than
// reading one frame in isolation.
//
// A cell holds a whole codepoint, not a byte. mux's chrome is full of box
// drawing and marks, and counting their bytes as columns would put everything
// after them in the wrong place — a row would look full several columns before
// the terminal thought so.

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#define ROWS_MAX 64
#define COLS_MAX 256
#define CELL_MAX 16             /* a UTF-8 codepoint, its marks and a terminator */

struct screen {
    int  rows, cols;
    char cell[ROWS_MAX][COLS_MAX + 1][CELL_MAX];
    int  cur_r, cur_c;
    int  cursor_visible;
    int  top, bot;              /* scroll region, 0-based inclusive */
};

__attribute__((unused)) static void row_blank_out(struct screen *s, int r, int from)
{
    for (int c = from; c < s->cols; c++) {
        s->cell[r][c][0] = ' ';
        s->cell[r][c][1] = '\0';
    }
}

__attribute__((unused)) static void screen_init(struct screen *s, int rows, int cols)
{
    memset(s, 0, sizeof *s);
    s->rows = rows;
    s->cols = cols;
    s->top = 0;
    s->bot = rows - 1;
    for (int r = 0; r < rows; r++)
        row_blank_out(s, r, 0);
}

// Scroll the region by n rows: positive moves content toward the top, which is
// what ESC[nS does, and negative is ESC[nT.
__attribute__((unused)) static void scroll_region(struct screen *s, int n)
{
    if (n == 0)
        return;
    int height = s->bot - s->top + 1;
    if (height <= 0)
        return;
    if (n > height || -n > height)
        n = n > 0 ? height : -height;

    size_t span = (size_t)s->cols * CELL_MAX;
    if (n > 0) {
        for (int r = s->top; r <= s->bot; r++) {
            int from = r + n;
            if (from <= s->bot)
                memcpy(s->cell[r], s->cell[from], span);
            else
                row_blank_out(s, r, 0);
        }
    } else {
        for (int r = s->bot; r >= s->top; r--) {
            int from = r + n;
            if (from >= s->top)
                memcpy(s->cell[r], s->cell[from], span);
            else
                row_blank_out(s, r, 0);
        }
    }
}

// One codepoint into one cell. Wrapping is off in every paint the viewport
// makes, so anything past the last column is dropped the way the terminal
// drops it. A combining mark takes no column of its own: it joins the cell
// before it, which is what makes an image's placeholder cells - a base
// codepoint and two diacritics - one column each rather than three.
static int is_combining(const char *p, size_t n)
{
    unsigned cp = 0;
    if (n == 2 && ((unsigned char)p[0] & 0xE0) == 0xC0)
        cp = (unsigned)(p[0] & 0x1F) << 6 | (p[1] & 0x3F);
    else if (n == 3 && ((unsigned char)p[0] & 0xF0) == 0xE0)
        cp = (unsigned)(p[0] & 0x0F) << 12 | (unsigned)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
    else
        return 0;
    return wcwidth((wchar_t)cp) == 0;
}

__attribute__((unused)) static void screen_put(struct screen *s, const char *p, size_t n)
{
    if (s->cur_r < 0 || s->cur_r >= s->rows || s->cur_c < 0 || s->cur_c >= s->cols) {
        if (!is_combining(p, n))
            s->cur_c++;
        return;
    }
    if (is_combining(p, n)) {
        if (s->cur_c == 0)
            return;
        char  *cell = s->cell[s->cur_r][s->cur_c - 1];
        size_t at = strlen(cell);
        if (at + n < CELL_MAX) {
            memcpy(cell + at, p, n);
            cell[at + n] = '\0';
        }
        return;
    }
    if (n > CELL_MAX - 1)
        n = CELL_MAX - 1;
    memcpy(s->cell[s->cur_r][s->cur_c], p, n);
    s->cell[s->cur_r][s->cur_c][n] = '\0';
    s->cur_c++;
}

static size_t utf8_cell(const char *p, size_t n)
{
    unsigned char c = (unsigned char)p[0];
    size_t want = 1;
    if ((c & 0xE0) == 0xC0)
        want = 2;
    else if ((c & 0xF0) == 0xE0)
        want = 3;
    else if ((c & 0xF8) == 0xF0)
        want = 4;
    return want <= n ? want : 1;
}

// Only what the viewport emits: absolute placement, erase-to-end-of-line, the
// scroll region and text. Anything else would be a change this model has to
// learn about.
__attribute__((unused)) static void feed(struct screen *s, const char *p, size_t n)
{
    for (size_t i = 0; i < n;) {
        if (p[i] == 0x1b && i + 1 < n && p[i + 1] == '[') {
            size_t j = i + 2;
            int    args[2] = {0, 0}, argc = 0;
            int    priv = j < n && p[j] == '?';
            if (priv)
                j++;
            while (j < n) {
                if (p[j] >= '0' && p[j] <= '9') {
                    if (!argc)
                        argc = 1;
                    if (argc <= 2)
                        args[argc - 1] = args[argc - 1] * 10 + (p[j] - '0');
                    j++;
                } else if (p[j] == ';') {
                    argc = argc < 2 ? argc + 1 : argc;
                    j++;
                } else {
                    break;
                }
            }
            if (j < n && priv && args[0] == 25) {
                if (p[j] == 'h')
                    s->cursor_visible = 1;
                else if (p[j] == 'l')
                    s->cursor_visible = 0;
            }
            if (j < n && !priv) {
                if (p[j] == 'H') {
                    s->cur_r = (argc > 0 && args[0] > 0 ? args[0] : 1) - 1;
                    s->cur_c = (argc > 1 && args[1] > 0 ? args[1] : 1) - 1;
                } else if (p[j] == 'K' && s->cur_r >= 0 && s->cur_r < s->rows) {
                    row_blank_out(s, s->cur_r, s->cur_c);
                } else if (p[j] == 'r') {
                    s->top = argc > 0 && args[0] > 0 ? args[0] - 1 : 0;
                    s->bot = argc > 1 && args[1] > 0 ? args[1] - 1 : s->rows - 1;
                    if (s->bot >= s->rows)
                        s->bot = s->rows - 1;
                } else if (p[j] == 'S') {
                    scroll_region(s, argc > 0 && args[0] > 0 ? args[0] : 1);
                } else if (p[j] == 'T') {
                    scroll_region(s, -(argc > 0 && args[0] > 0 ? args[0] : 1));
                }
            }
            i = j < n ? j + 1 : n;
            continue;
        }
        if (p[i] == 0x1b) {
            // OSC and friends: skip to the terminator.
            size_t j = i + 1;
            while (j < n && p[j] != 0x07 && !(p[j] == 0x1b && j > i + 1))
                j++;
            i = j < n ? j + 1 : n;
            continue;
        }
        size_t w = utf8_cell(p + i, n - i);
        screen_put(s, p + i, w);
        i += w;
    }
}

// The text of one row, NUL-terminated.
__attribute__((unused)) static const char *row_text(const struct screen *s, int r)
{
    static char row[(COLS_MAX + 1) * CELL_MAX];
    size_t at = 0;
    if (r < 0 || r >= s->rows)
        return "";
    for (int c = 0; c < s->cols; c++) {
        size_t len = strlen(s->cell[r][c]);
        memcpy(row + at, s->cell[r][c], len);
        at += len;
    }
    row[at] = '\0';
    return row;
}

__attribute__((unused)) static int count_on_screen(const struct screen *s, const char *needle)
{
    int hits = 0;
    for (int r = 0; r < s->rows; r++)
        if (strstr(row_text(s, r), needle))
            hits++;
    return hits;
}

__attribute__((unused)) static int row_blank(const struct screen *s, int r)
{
    const char *t = row_text(s, r);
    for (; *t; t++)
        if (*t != ' ')
            return 0;
    return 1;
}

// The first row whose text contains `needle`, or -1.
__attribute__((unused)) static int row_with(const struct screen *s, const char *needle)
{
    for (int r = 0; r < s->rows; r++)
        if (strstr(row_text(s, r), needle))
            return r;
    return -1;
}

#endif
