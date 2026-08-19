
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "prompt.h"
#include "status.h"
#include "tty.h"
#include "restart.h"
#include "ui.h"

void restart_shield_thread(void) {}

int main(void)
{
    ui_init();
    ui_raw(1);
    tty_raw_begin();

    struct prompt *p = prompt_new(NULL, 0);
    status_set_below(prompt_live_paint, p);
    status_set_above(prompt_queue_paint, p);

    int chunk = getenv("CHUNK") ? atoi(getenv("CHUNK")) : 12;
    int chunks = getenv("CHUNKS") ? atoi(getenv("CHUNKS")) : 0;
    int sent = 0;
    int fill = getenv("FILL") ? atoi(getenv("FILL")) : 6;
    for (int i = 0; i < fill; i++) {
        ui_printf("transcript line %d: some text that may be long enough to wrap "
                  "when the terminal gets narrower than it was\n", i);
    }
    ui_flush();

    const char *sticky = getenv("STICKY");
    if (sticky && *sticky) {
        status_sticky_set(1);
        prompt_echo_message(sticky);
        status_sticky_prompt(sticky);
    }

    status_begin();
    for (int i = 0; i < 1600; i++) {
        tty_event ev;
        while (tty_read(&ev, 0)) {
            status_touch();
            if (ev.key == TK_TEXT)
                free(ev.text);
            if (ev.key == TK_EOF || (ev.key == TK_CHAR && ev.cp == 3))
                goto done;
        }
        if (chunk > 0 && i % chunk == chunk - 1) {
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

    if (p) {
        for (;;) {
            char *line = prompt_read(p);
            if (!line)
                break;
            free(line);
        }
        prompt_free(p);
    }
    status_set_below(NULL, NULL);
    status_set_above(NULL, NULL);
    tty_raw_end();
    return 0;
}
