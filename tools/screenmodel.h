#ifndef SCREENMODEL_H
#define SCREENMODEL_H

// A model of the screen the viewport paints onto, applying only the sequences
// it emits. The painter sends just what changed, so a test that wants to know
// what is on screen has to keep the screen the way a terminal does rather than
// reading one frame in isolation.

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define ROWS_MAX 64
#define COLS_MAX 256


struct screen {
    int  rows, cols;
    char cell[ROWS_MAX][COLS_MAX + 1];
    int  cur_r, cur_c;
    int  top, bot;              /* scroll region, 0-based inclusive */
};

__attribute__((unused)) static void screen_init(struct screen *s, int rows, int cols)
{
    memset(s, 0, sizeof *s);
    s->rows = rows;
    s->cols = cols;
    s->top = 0;
    s->bot = rows - 1;
    for (int r = 0; r < rows; r++)
        memset(s->cell[r], ' ', (size_t)cols);
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

    if (n > 0) {
        for (int r = s->top; r <= s->bot; r++) {
            int from = r + n;
            if (from <= s->bot)
                memcpy(s->cell[r], s->cell[from], (size_t)s->cols);
            else
                memset(s->cell[r], ' ', (size_t)s->cols);
        }
    } else {
        for (int r = s->bot; r >= s->top; r--) {
            int from = r + n;
            if (from >= s->top)
                memcpy(s->cell[r], s->cell[from], (size_t)s->cols);
            else
                memset(s->cell[r], ' ', (size_t)s->cols);
        }
    }
}

// Only what the viewport emits: absolute placement, erase-to-end-of-line, and
// text. Anything else would be a change this model has to learn about.
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
            if (j < n && !priv) {
                if (p[j] == 'H') {
                    s->cur_r = (argc > 0 && args[0] > 0 ? args[0] : 1) - 1;
                    s->cur_c = (argc > 1 && args[1] > 0 ? args[1] : 1) - 1;
                } else if (p[j] == 'K' && s->cur_r < s->rows) {
                    for (int c = s->cur_c; c < s->cols; c++)
                        s->cell[s->cur_r][c] = ' ';
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
        if (s->cur_r >= 0 && s->cur_r < s->rows && s->cur_c >= 0 && s->cur_c < s->cols)
            s->cell[s->cur_r][s->cur_c] = p[i];
        s->cur_c++;
        i++;
    }
}
__attribute__((unused)) static int count_on_screen(const struct screen *s, const char *needle)
{
    int hits = 0;
    for (int r = 0; r < s->rows; r++) {
        char row[COLS_MAX + 1];
        memcpy(row, s->cell[r], (size_t)s->cols);
        row[s->cols] = '\0';
        if (strstr(row, needle))
            hits++;
    }
    return hits;
}

// The text of one row, NUL-terminated, for tests that care where a row sits
// rather than only whether it is present.
__attribute__((unused)) static const char *row_text(const struct screen *s, int r)
{
    static char row[COLS_MAX + 1];
    if (r < 0 || r >= s->rows)
        return "";
    memcpy(row, s->cell[r], (size_t)s->cols);
    row[s->cols] = '\0';
    return row;
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
