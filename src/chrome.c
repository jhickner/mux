#include "chrome.h"

#include "block.h"
#include "prompt.h"
#include "sidechannel.h"
#include "status.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"

static struct prompt   *bound;
static chrome_modal_fn  modal;
static void            *modal_ud;

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

static int (*modal_interrupt)(void);

void chrome_modal_interrupt(int (*fn)(void))
{
    modal_interrupt = fn;
}

int chrome_modal_interrupted(void)
{
    return modal_interrupt ? modal_interrupt() : 0;
}

int chrome_modal_active(void)
{
    return modal != NULL;
}

void chrome_modal(chrome_modal_fn fn, void *ud)
{
    modal = fn;
    modal_ud = ud;
    chrome_paint();
}

// The sections above the input, in draw order. Dropped whole, cheapest first,
// until what is left fits.
struct above {
    int side;
    int sticky;
    int queued;
};

// Their heights, measured once a frame. Measuring is not cheap — the sticky
// one walks the visible transcript — and dropping a section cannot change what
// another one measures, so the fit reads these instead of asking again.
struct heights {
    int side;
    int sticky;
    int queued;
};

static struct heights above_measure(int cols)
{
    struct heights h;
    h.side = sidechannel_rows();
    h.sticky = status_sticky_measure();
    h.queued = prompt_queued_rows(bound, cols);
    return h;
}

// Each section that draws anything is set off from the one above it.
static int above_height(const struct above *a, const struct heights *h)
{
    int rows = 0;
    int drawn = 0;

    if (a->side && h->side > 0)
        rows += h->side + (drawn++ ? 1 : 0);
    if (a->sticky && h->sticky > 0)
        rows += h->sticky + (drawn++ ? 1 : 0);
    if (a->queued && h->queued > 0)
        rows += h->queued + (drawn++ ? 1 : 0);
    return rows ? rows + 1 : 0;
}

static void fit_above(struct above *a, const struct heights *h, int room)
{
    if (above_height(a, h) <= room)
        return;
    a->queued = 0;
    if (above_height(a, h) <= room)
        return;
    a->sticky = 0;
    if (above_height(a, h) <= room)
        return;
    a->side = 0;
}

// The block sits one blank row clear of the transcript, unless the transcript
// already ends in one.
int chrome_gap(void)
{
    return viewport_active() && !viewport_ends_blank();
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
        if (chrome_gap())
            ui_put("\n");
        modal(modal_ud);
        block_end(0, -1);
        return;
    }

    if (!bound) {
        chrome_clear();
        return;
    }

    int cols = ui_columns();
    int spinning = status_spinning();

    // The input is never dropped, so it is measured first and the rest fitted
    // into what it leaves.
    int input_rows = prompt_input_rows(bound, cols);
    int gap = chrome_gap();

    struct heights h = above_measure(cols);
    struct above a = {1, 1, 1};
    fit_above(&a, &h, tty_rows() - 1 - input_rows - spinning - gap);

    block_begin();
    budget = tty_rows() - 1;
    spin_row = -1;

    if (gap)
        ui_put("\n");

    int drawn = 0;
    if (a.side && h.side > 0) {
        sidechannel_paint(chrome_rows_left());
        drawn = 1;
    }
    if (a.sticky && h.sticky > 0) {
        if (drawn)
            ui_put("\n");
        status_paint_sticky();
        drawn = 1;
    }
    if (a.queued && h.queued > 0) {
        if (drawn)
            ui_put("\n");
        prompt_paint_queued(bound, chrome_rows_left());
    }

    // A blank row under what is pinned above, off the spinner.
    if (ui_sink_rows() - gap > 0) {
        ui_esc(UI_ERASE_EOL);
        ui_put("\n");
    }

    // Counted off what was painted, not what the fit predicted.
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
