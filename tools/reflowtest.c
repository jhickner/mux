
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

    if (failures == 0)
        printf("reflowtest: all checks passed\n");
    return failures != 0;
}
