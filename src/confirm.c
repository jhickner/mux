#include "confirm.h"

#include <stdlib.h>

#include "block.h"
#include "tty.h"
#include "ui.h"

// The question is chrome like the prompt is: painted under the transcript and
// repainted whole, so a resize needs nothing but another paint.
static void paint_question(const char *question)
{
    block_begin();
    ui_esc(ui_style(UI_CHROME));
    ui_put(UI_BAR);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");
    ui_put(question);
    ui_put(" ");
    ui_esc(ui_style(UI_ACCENT));
    ui_put("y/n");
    ui_esc(ui_style(UI_RESET));
    block_end(0, -1);
}

int confirm_run(const char *question)
{
    if (!question || !*question)
        return 0;

    paint_question(question);
    for (;;) {
        tty_event ev;
        if (!tty_read(&ev, -1))
            continue;
        if (ev.key == TK_TEXT) {
            free(ev.text);
            continue;
        }
        if (ev.key == TK_CHAR && (ev.cp == 'y' || ev.cp == 'Y')) {
            block_clear();
            return 1;
        }
        if ((ev.key == TK_CHAR && (ev.cp == 'n' || ev.cp == 'N' ||
                                   ev.cp == 3 || ev.cp == 4)) ||
            ev.key == TK_ESCAPE || ev.key == TK_EOF) {
            block_clear();
            return 0;
        }
        if (ev.key == TK_RESIZE)
            paint_question(question);
    }
}
