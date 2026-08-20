#include "pick.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chrome.h"
#include "tty.h"
#include "ui.h"

#define VISIBLE_MAX 10

struct view {
    int top;
    int visible;
    const char *title;
    const struct pick_item *items;
    int count;
    int sel;
};

// A modal section: chrome.c paints it in place of the whole stack.
static void paint(void *ud)
{
    struct view *v = ud;
    const char *title = v->title;
    const struct pick_item *items = v->items;
    int count = v->count, sel = v->sel;

    if (sel < v->top)
        v->top = sel;
    if (sel >= v->top + v->visible)
        v->top = sel - v->visible + 1;

    int columns = ui_columns();
    int rows = 0;

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
    ui_put("\n");
    rows++;

    int end = v->top + v->visible;
    if (end > count)
        end = count;
    for (int i = v->top; i < end; i++) {
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
        ui_put("\n");
        rows++;
    }

    if (count > v->visible) {
        ui_esc(ui_style(UI_DIM));
        ui_printf("    %d\u2013%d of %d", v->top + 1, end, count);
        ui_esc(ui_style(UI_RESET));
        ui_put("\n");
        rows++;
    }

    (void)rows;
}

int pick_run(const char *title, const struct pick_item *items, int count, int initial)
{
    return pick_run_keys(title, items, count, initial, NULL, NULL);
}

int pick_run_keys(const char *title, const struct pick_item *items, int count,
                  int initial, const char *shortcuts, int *pressed)
{
    if (pressed)
        *pressed = 0;
    if (count <= 0)
        return -1;

    struct view v = {0};
    v.visible = count < VISIBLE_MAX ? count : VISIBLE_MAX;
    v.title = title;
    v.items = items;
    v.count = count;
    v.sel = (initial >= 0 && initial < count) ? initial : 0;
    chrome_modal(paint, &v);

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
            v.sel = v.sel > 0 ? v.sel - 1 : count - 1;
            break;
        case TK_DOWN:
            v.sel = v.sel + 1 < count ? v.sel + 1 : 0;
            break;
        case TK_HOME:
            v.sel = 0;
            break;
        case TK_END:
            v.sel = count - 1;
            break;
        case TK_ENTER: {
            int chosen = v.sel;
            chrome_modal(NULL, NULL);
            return chosen;
        }
        case TK_ESCAPE:
        case TK_EOF:
            chrome_modal(NULL, NULL);
            return -1;
        case TK_CHAR:
            if (ev.cp == 3 || ev.cp == 4) {
                chrome_modal(NULL, NULL);
                return -1;
            }

            if (shortcuts && ev.cp > 0 && ev.cp < 128 &&
                strchr(shortcuts, (int)ev.cp)) {
                int chosen = v.sel;
                chrome_modal(NULL, NULL);
                if (pressed)
                    *pressed = (int)ev.cp;
                return chosen;
            }

            if (ev.cp >= '1' && ev.cp <= '9' && (int)(ev.cp - '1') < count)
                v.sel = (int)(ev.cp - '1');
            break;
        case TK_RESIZE:
            break;
        default:
            continue;
        }
        chrome_paint();
    }
}
