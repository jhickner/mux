/* Harness: drives status.c the way a live turn does, so resize artifacts can be
 * reproduced under a scripted tmux session. Not part of the app. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "status.h"
#include "tty.h"
#include "ui.h"

static void below(void *ud, int *rows, int *caret_row, int *caret_col)
{
    (void)ud;
    int extra = getenv("WIDE") ? 1 : 0;
    fputs("\x1b[K", stdout);
    fputs("\xe2\x9d\xaf ", stdout);
    if (extra) {
        for (int i = 0; i < 200; i++)
            fputc('x', stdout);
    }
    *rows = 1;
    *caret_row = 0;
    *caret_col = 2;
}

int main(void)
{
    ui_init();
    ui_raw(1);
    tty_raw_begin();
    status_set_below(below, NULL, NULL);

    int chunk = getenv("CHUNK") ? atoi(getenv("CHUNK")) : 12;
    int fill = getenv("FILL") ? atoi(getenv("FILL")) : 6;
    for (int i = 0; i < fill; i++) {
        ui_printf("transcript line %d: some text that may be long enough to wrap "
                  "when the terminal gets narrower than it was\n", i);
    }
    ui_flush();

    status_begin();
    for (int i = 0; i < 400; i++) {
        tty_event ev;
        while (tty_read(&ev, 0)) {
            if (ev.key == TK_TEXT)
                free(ev.text);
            if (ev.key == TK_EOF || (ev.key == TK_CHAR && ev.cp == 3))
                goto done;
        }
        if (chunk > 0 && i % chunk == chunk - 1) { /* a streamed chunk, as a turn does */
            status_pause();
            ui_printf("assistant chunk %d: now let me make the edits, this line is "
                      "long enough to rewrap on a narrow terminal\n\n", i);
            ui_flush();
            status_resume();
        }
        status_tick();
        usleep(80000);
    }
done:
    status_end();
    tty_raw_end();
    return 0;
}
