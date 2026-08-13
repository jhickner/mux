#include "status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "ui.h"

static const char *const FRAMES[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
#define FRAME_COUNT ((int)(sizeof FRAMES / sizeof *FRAMES))

/* The spinner advances on its own clock so the animation stays smooth however
 * often the caller happens to tick. */
#define FRAME_MS 90

static double  started;
static int     visible;
static int     frame;
static double  frame_at;
static char   *label;

static double now_seconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

double status_elapsed(void) { return now_seconds() - started; }

static void erase_line(void)
{
    ui_esc("\r\x1b[K");
    ui_flush();
}

static void paint(void)
{
    double elapsed = status_elapsed();
    char left[64];
    snprintf(left, sizeof left, "%s %.0fs", FRAMES[frame], elapsed);

    erase_line();
    ui_esc(ui_style(UI_DIM));
    ui_put(left);
    if (label && *label) {
        ui_put(" · ");
        /* Keep the line to one row so the next erase clears all of it. */
        int budget = ui_columns() - (int)ui_cells(left) - 4;
        if (budget < 1)
            budget = 1;
        size_t skip = 0;
        size_t fit = ui_wrap_row(label, (size_t)budget, &skip);
        ui_putn(label, fit);
        if (label[fit])
            ui_put("…");
    }
    ui_esc(ui_style(UI_RESET));
    ui_flush();
}

void status_begin(void)
{
    started = now_seconds();
    frame = 0;
    frame_at = started;
    visible = 1;
    free(label);
    label = NULL;
    ui_esc("\x1b[?25l");
    paint();
}

void status_tick(void)
{
    if (!visible)
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
    erase_line();
    visible = 0;
}

void status_resume(void)
{
    if (visible)
        return;
    visible = 1;
    paint();
}

void status_activity(const char *text)
{
    free(label);
    label = text ? strdup(text) : NULL;
    if (visible)
        paint();
}

void status_end(void)
{
    if (visible)
        erase_line();
    visible = 0;
    free(label);
    label = NULL;
    ui_esc("\x1b[?25h");
    ui_flush();
}
