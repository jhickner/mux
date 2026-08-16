
#ifndef TTY_H
#define TTY_H

#include <stddef.h>
#include <stdint.h>

/* Forward-declared rather than including <termios.h>: session.h includes this
   header, so every session consumer would otherwise pull in termios. */
struct termios;

typedef enum {
    TK_CHAR,
    TK_TEXT,
    TK_ENTER,
    TK_NEWLINE,
    TK_BACKSPACE,
    TK_DELETE,
    TK_TAB,
    TK_ESCAPE,
    TK_LEFT,
    TK_RIGHT,
    TK_UP,
    TK_DOWN,
    TK_WORD_LEFT,
    TK_WORD_RIGHT,
    TK_HOME,
    TK_END,
    TK_RESIZE,
    TK_EOF,
} tty_key;

typedef struct {
    tty_key   key;
    uint32_t  cp;
    char     *text;
} tty_event;

int  tty_raw_begin(void);

void tty_raw_end(void);

int  tty_read(tty_event *ev, int timeout_ms);

void tty_watch(int (*fd)(void *ud), void (*ready)(void *ud), void *ud);

int  tty_is_raw(void);

int tty_cooked_termios(struct termios *out);

size_t tty_take_pending(void *buf, size_t max);

unsigned tty_resize_epoch(void);

#define TTY_RESIZE_SETTLE_MS 250

int  tty_columns(void);

int  tty_rows(void);

#endif
