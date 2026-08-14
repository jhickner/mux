/* Harness: drives status.c the way a live turn does, so resize artifacts can be
 * reproduced under a scripted tmux session. Not part of the app. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "prompt.h"
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
    int chunks = getenv("CHUNKS") ? atoi(getenv("CHUNKS")) : 0; /* end after n */
    int sent = 0;
    int fill = getenv("FILL") ? atoi(getenv("FILL")) : 6;
    for (int i = 0; i < fill; i++) {
        ui_printf("transcript line %d: some text that may be long enough to wrap "
                  "when the terminal gets narrower than it was\n", i);
    }
    ui_flush();

    /* One turn as the app runs it: the message is echoed into scrollback, then
     * carried above the spinner, and the idle prompt takes it over at the end. */
    const char *sticky = getenv("STICKY");
    if (sticky && *sticky) {
        status_sticky_set(1);
        prompt_echo_message(sticky);
        status_sticky_prompt(sticky);
    }

    status_begin();
    for (int i = 0; i < 1600; i++) { /* the drivers poll about every 20ms */
        tty_event ev;
        while (tty_read(&ev, 0)) {
            status_touch();
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
            if (chunks && ++sent >= chunks)
                goto done;
        }
        status_tick();
        usleep(20000);
    }
done:
    status_end();
    status_set_below(NULL, NULL, NULL);

    /* The idle prompt, which retains the message once its echo has scrolled
     * away. Reads until ctrl-d. */
    struct prompt *p = prompt_new(NULL, 0);
    if (p) {
        free(prompt_read(p));
        prompt_free(p);
    }
    tty_raw_end();
    return 0;
}
