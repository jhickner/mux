
#include <stdio.h>

#include "ui.h"

static int failures;

static void expect(int got, int want, const char *what)
{
    if (got != want) {
        printf("FAIL %s: got %d, want %d\n", what, got, want);
        failures++;
    }
}

int main(void)
{

    int fits = 40;
    expect(ui_reflow_rows(&fits, 1, 80), 1, "40 cells at 80 cols");
    expect(ui_reflow_rows(&fits, 1, 40), 1, "40 cells at 40 cols (exact fit)");

    expect(ui_reflow_rows(&fits, 1, 39), 2, "40 cells at 39 cols");
    expect(ui_reflow_rows(&fits, 1, 20), 2, "40 cells at 20 cols");
    expect(ui_reflow_rows(&fits, 1, 14), 3, "40 cells at 14 cols");

    int wide = 99;
    expect(ui_reflow_rows(&wide, 1, 100), 1, "99 cells at 100 cols");
    expect(ui_reflow_rows(&wide, 1, 60), 2, "99 cells at 60 cols");
    expect(ui_reflow_rows(&wide, 1, 33), 3, "99 cells at 33 cols");

    int rows[] = {10, 0, 10};
    expect(ui_reflow_rows(rows, 3, 80), 3, "blank row counts as one");
    expect(ui_reflow_rows(rows, 3, 5), 5, "blank row among wrapped rows");

    expect(ui_reflow_rows(rows, 0, 80), 0, "no rows");

    expect(ui_reflow_rows(rows, 3, 0), 3, "zero cols falls back to one each");

    /* Cell widths. These are wrong in the startup "C" locale, where wcwidth
       returns -1 for every non-ASCII codepoint, so they also guard the
       setlocale call in ui_init. */
    ui_init();
    expect((int)ui_cells("abc"), 3, "ascii is one cell each");
    expect((int)ui_cells("\xe4\xb8\xad"), 2, "CJK is two cells");           /* 中 */
    expect((int)ui_cells("\xe2\x9c\x93"), 1, "check mark is one cell");     /* ✓ */
    expect((int)ui_cells("\xe2\x94\x82"), 1, "box drawing is one cell");    /* │ */
    expect((int)ui_cells("\xe2\x96\x8c"), 1, "the bar glyph is one cell");  /* ▌ */
    expect((int)ui_cells("\xf0\x9f\x98\x80"), 2, "emoji is two cells");     /* 😀 */
    expect((int)ui_cells("\xe2\x9c\x85"), 2, "emoji-presentation is two");  /* ✅ */
    expect((int)ui_cells(NULL), 0, "NULL measures zero");

    /* A glyph wider than the budget must still be consumed, or the callers'
       `p += row + skip` walk spins forever. */
    size_t skip = 0;
    expect((int)ui_wrap_row("\xe4\xb8\xad", 1, &skip), 3, "too-wide glyph is consumed");
    expect((int)skip, 0, "and reports no skip");

    struct ui_wrap w = {0};
    w.budget = 10;
    w.measure = 1;
    w.paint_empty = 1;
    expect(ui_wrap_paint("", &w), 1, "empty text is one row");
    expect(ui_wrap_paint("hello", &w), 1, "text inside the budget is one row");
    expect(ui_wrap_paint("hello world again", &w), 3, "wraps on word boundaries");
    w.max_rows = 1;
    expect(ui_wrap_paint("hello world again", &w), 1, "max_rows clips");

    /* The gutter and first-row mark must be counted, or a resize moves the
       cursor up too few rows. */
    int widths[4];
    w.max_rows = 0;
    w.gutter = "| ";
    w.mark = "* ";
    w.widths = widths;
    w.widths_max = 4;
    expect(ui_wrap_paint("abc", &w), 1, "one row with a gutter");
    expect(widths[0], 2 + 2 + 3, "gutter and mark counted on the first row");

    if (failures == 0)
        printf("reflowtest: all checks passed\n");
    return failures != 0;
}
