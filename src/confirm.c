#include "confirm.h"

#include <stdlib.h>

#include "chrome.h"
#include "tty.h"
#include "ui.h"

// A modal section: chrome.c paints it in place of the whole stack, so a resize
// needs nothing but another paint.
static void paint_question(void *ud)
{
    const char *question = ud;

    ui_esc(ui_style(UI_CHROME));
    ui_put(UI_BAR);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");
    ui_put(question);
    ui_put(" ");
    ui_esc(ui_style(UI_ACCENT));
    ui_put("y/n");
    ui_esc(ui_style(UI_RESET));
}

int confirm_run(const char *question)
{
    if (!question || !*question)
        return 0;

    chrome_modal(paint_question, (void *)question);
    for (;;) {
        tty_event ev;
        if (!tty_read(&ev, -1))
            continue;
        if (ev.key == TK_TEXT) {
            free(ev.text);
            continue;
        }
        if (ev.key == TK_CHAR && (ev.cp == 'y' || ev.cp == 'Y')) {
            chrome_modal(NULL, NULL);
            return 1;
        }
        if ((ev.key == TK_CHAR && (ev.cp == 'n' || ev.cp == 'N' ||
                                   ev.cp == 3 || ev.cp == 4)) ||
            ev.key == TK_ESCAPE || ev.key == TK_EOF) {
            chrome_modal(NULL, NULL);
            return 0;
        }
        if (ev.key == TK_RESIZE)
            chrome_paint();
    }
}
