#include "pick.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tty.h"
#include "ui.h"

#define VISIBLE_MAX 10

struct view {
    int rows;
    int top;
    int visible;
};

static void erase(struct view *v)
{
    if (v->rows == 0)
        return;
    printf("\x1b[%dA\r\x1b[J", v->rows);
    fflush(stdout);
    v->rows = 0;
}

static void paint(struct view *v, const char *title, const struct pick_item *items, int count,
                  int sel)
{
    if (sel < v->top)
        v->top = sel;
    if (sel >= v->top + v->visible)
        v->top = sel - v->visible + 1;

    if (v->rows > 0)
        printf("\x1b[%dA\r", v->rows);
    fputs(UI_CURSOR_HIDE, stdout);
    ui_scroll_track(0);

    int columns = ui_columns();
    int rows = 0;

    fputs(UI_ERASE_EOL, stdout);
    ui_esc(ui_style(UI_CHROME));
    ui_put(UI_BAR);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");
    ui_esc(ui_style(UI_DIM));
    {
        size_t title_budget = columns > 3 ? (size_t)(columns - 3) : 1;
        size_t title_n = ui_fit_bytes(title, title_budget);
        ui_putn(title, title_n);
        if (title[title_n])
            ui_put("…");
    }
    ui_esc(ui_style(UI_RESET));
    ui_put("\r\n");
    rows++;

    int end = v->top + v->visible;
    if (end > count)
        end = count;
    for (int i = v->top; i < end; i++) {
        fputs(UI_ERASE_EOL, stdout);
        int selected = (i == sel);
        ui_esc(ui_style(selected ? UI_ACCENT : UI_RESET));
        ui_put(selected ? "  \xe2\x86\x92 " : "    ");

        size_t label_budget = columns > 5 ? (size_t)(columns - 5) : 1;
        size_t label_n = ui_fit_bytes(items[i].label, label_budget);
        ui_putn(items[i].label, label_n);
        if (items[i].label[label_n])
            ui_put("…");
        ui_esc(ui_style(UI_RESET));

        size_t used = 4 + ui_cells_n(items[i].label, label_n) +
                      (items[i].label[label_n] ? 1 : 0);

        if (items[i].detail && *items[i].detail) {
            int budget = columns - (int)used - 4;
            if (budget > 6) {
                ui_put("  ");
                ui_esc(ui_style(UI_DIM));
                size_t skip = 0;
                size_t fit = ui_wrap_row(items[i].detail, strlen(items[i].detail),
                                         (size_t)budget, &skip, NULL);
                ui_putn(items[i].detail, fit);
                if (items[i].detail[fit])
                    ui_put("…");
                ui_esc(ui_style(UI_RESET));
            }
        }
        ui_put("\r\n");
        rows++;
    }

    if (count > v->visible) {
        fputs(UI_ERASE_EOL, stdout);
        ui_esc(ui_style(UI_DIM));
        ui_printf("    %d–%d of %d", v->top + 1, end, count);
        ui_esc(ui_style(UI_RESET));
        ui_put("\r\n");
        rows++;
    }

    fputs("\x1b[J\x1b[?25h", stdout);
    ui_scroll_track(1);
    fflush(stdout);
    v->rows = rows;
}

int pick_run(const char *title, const struct pick_item *items, int count, int initial)
{
    if (count <= 0)
        return -1;

    struct view v = {0};
    v.visible = count < VISIBLE_MAX ? count : VISIBLE_MAX;
    int sel = (initial >= 0 && initial < count) ? initial : 0;
    paint(&v, title, items, count, sel);

    for (;;) {
        tty_event ev;
        if (!tty_read(&ev, -1))
            continue;
        if (ev.key == TK_TEXT) {
            free(ev.text);
            continue;
        }
        switch (ev.key) {
        case TK_UP:
            sel = sel > 0 ? sel - 1 : count - 1;
            break;
        case TK_DOWN:
            sel = sel + 1 < count ? sel + 1 : 0;
            break;
        case TK_HOME:
            sel = 0;
            break;
        case TK_END:
            sel = count - 1;
            break;
        case TK_ENTER:
            erase(&v);
            return sel;
        case TK_ESCAPE:
        case TK_EOF:
            erase(&v);
            return -1;
        case TK_CHAR:
            if (ev.cp == 3 || ev.cp == 4) {
                erase(&v);
                return -1;
            }

            if (ev.cp >= '1' && ev.cp <= '9' && (int)(ev.cp - '1') < count)
                sel = (int)(ev.cp - '1');
            break;
        case TK_RESIZE:
            break;
        default:
            continue;
        }
        paint(&v, title, items, count, sel);
    }
}
