
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chrome.h"
#include "prompt.h"
#include "sidechannel.h"
#include "status.h"
#include "tty.h"
#include "restart.h"
#include "ui.h"

void restart_shield_thread(void) {}

// sidechannel is stubbed rather than linked: it would drag the session, the
// agent drivers and the markdown renderer in behind it.
int  sidechannel_rows(void) { return 0; }
void sidechannel_paint(int budget) { (void)budget; }
void sidechannel_tick(void) {}
void sidechannel_poll(void) {}
int  sidechannel_busy(void) { return 0; }
void sidechannel_close_all(void) {}
int  sidechannel_fds(int *out, int max) { (void)out; (void)max; return 0; }


int main(void)
{
    ui_init();
    ui_raw(1);
    tty_raw_begin();

    struct prompt *p = prompt_new(NULL, 0);
    chrome_bind(p);

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
    chrome_bind(NULL);
    tty_raw_end();
    return 0;
}
