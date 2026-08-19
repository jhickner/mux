#include "confirm.h"

#include <stdio.h>
#include <stdlib.h>

#include "block.h"
#include "tty.h"
#include "ui.h"

struct confirm_view {
    int rows;
};

static void erase_question(struct confirm_view *view)
{
    if (!view->rows)
        return;
    fprintf(stdout, "\x1b[%dA\r\x1b[J", view->rows);
    fflush(stdout);
    view->rows = 0;
    block_forget();
}

static void paint_question(struct confirm_view *view, const char *question)
{
    block_clear();

    if (view->rows)
        fprintf(stdout, "\x1b[%dA\r", view->rows);
    else
        fputc('\r', stdout);
    fputs(UI_CURSOR_HIDE UI_ERASE_EOL, stdout);
    ui_scroll_track(0);
    ui_esc(ui_style(UI_CHROME));
    ui_put(UI_BAR);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");
    ui_put(question);
    ui_put(" ");
    ui_esc(ui_style(UI_ACCENT));
    ui_put("y/n");
    ui_esc(ui_style(UI_RESET));
    ui_put("\r\n");
    fputs("\x1b[J" UI_CURSOR_SHOW, stdout);
    ui_scroll_track(1);
    fflush(stdout);

    size_t cells = 1 + 1 + ui_cells(question) + 1 + 3;
    int columns = ui_columns();
    view->rows = (int)((cells + (size_t)columns - 1) / (size_t)columns);
}

int confirm_run(const char *question)
{
    if (!question || !*question)
        return 0;

    struct confirm_view view = {0};
    paint_question(&view, question);
    for (;;) {
        tty_event ev;
        if (!tty_read(&ev, -1))
            continue;
        if (ev.key == TK_TEXT) {
            free(ev.text);
            continue;
        }
        if (ev.key == TK_CHAR && (ev.cp == 'y' || ev.cp == 'Y')) {
            erase_question(&view);
            return 1;
        }
        if ((ev.key == TK_CHAR && (ev.cp == 'n' || ev.cp == 'N' ||
                                   ev.cp == 3 || ev.cp == 4)) ||
            ev.key == TK_ESCAPE || ev.key == TK_EOF) {
            erase_question(&view);
            return 0;
        }
        if (ev.key == TK_RESIZE)
            paint_question(&view, question);
    }
}
