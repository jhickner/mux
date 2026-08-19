#include "chrome.h"

#include "block.h"
#include "prompt.h"
#include "sidechannel.h"
#include "status.h"
#include "tty.h"
#include "ui.h"

static struct prompt   *bound;
static chrome_modal_fn  modal;
static void            *modal_ud;

// Rows the whole block may spend, and where the spinner landed in it.
static int budget;
static int spin_row = -1;
static int above_rows;

void chrome_bind(struct prompt *p) { bound = p; }

int chrome_rows_left(void)
{
    int left = budget - ui_sink_rows();
    return left > 0 ? left : 0;
}

void chrome_clear(void)
{
    spin_row = -1;
    above_rows = 0;
    block_clear();
}

void chrome_keep_above(void)
{
    block_keep(above_rows);
    spin_row = -1;
    above_rows = 0;
}

void chrome_modal(chrome_modal_fn fn, void *ud)
{
    modal = fn;
    modal_ud = ud;
    chrome_paint();
}

// The sections above the input, in the order they are drawn. Each is dropped
// whole rather than clipped, cheapest first, until what is left fits over the
// input.
struct above {
    int sticky;
    int side;
    int queued;
};

static int above_height(const struct above *a, int busy, int cols)
{
    int rows = 0;
    if (a->sticky)
        rows += status_sticky_measure(busy);
    if (a->side)
        rows += sidechannel_rows();
    if (a->queued)
        rows += prompt_queued_rows(bound, cols);
    return rows;
}

static void fit_above(struct above *a, int busy, int cols, int room)
{
    if (above_height(a, busy, cols) <= room)
        return;
    a->queued = 0;
    if (above_height(a, busy, cols) <= room)
        return;
    a->sticky = 0;
    if (above_height(a, busy, cols) <= room)
        return;
    a->side = 0;
}

void chrome_paint(void)
{
    if (ui_too_narrow()) {
        chrome_clear();
        return;
    }

    if (modal) {
        block_begin();
        budget = tty_rows() - 1;
        spin_row = -1;
        above_rows = 0;
        modal(modal_ud);
        block_end(0, -1);
        return;
    }

    if (!bound) {
        chrome_clear();
        return;
    }

    int cols = ui_columns();
    int busy = prompt_busy(bound);
    int spinning = status_spinning();

    // The input is measured first: it is the one section that is never
    // dropped, so everything above it is fitted into what it leaves.
    int input_rows = prompt_input_rows(bound, cols);
    int gap = status_gap_row();

    struct above a = {1, 1, 1};
    fit_above(&a, busy, cols, tty_rows() - 1 - input_rows - spinning - gap);

    block_begin();
    budget = tty_rows() - 1;
    spin_row = -1;

    if (gap)
        ui_put("\n");
    if (a.sticky)
        status_paint_sticky(busy);
    if (a.side)
        sidechannel_paint(chrome_rows_left());
    if (a.queued)
        prompt_paint_queued(bound, chrome_rows_left());

    // Counted off what was painted rather than off what the fit predicted, so
    // the caret lands on the row it was actually drawn into.
    above_rows = ui_sink_rows() - gap;

    if (spinning) {
        spin_row = ui_sink_rows();
        status_paint_spin();
        ui_put("\n");
    }

    int caret_row = 0, caret_col = -1;
    int first = ui_sink_rows();
    prompt_paint_input(bound, input_rows, &caret_row, &caret_col);

    block_end(first + caret_row, caret_col);
}

int chrome_paint_spin(void)
{
    if (spin_row < 0 || !status_spinning() || ui_columns() < 24 || !block_have())
        return 0;
    block_row_begin(spin_row);
    status_paint_spin();
    block_row_end();
    return 1;
}
