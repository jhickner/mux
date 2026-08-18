
#ifndef TTY_H
#define TTY_H

#include <stddef.h>
#include <stdint.h>
#include <termios.h>

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
    TK_PAGE_UP,
    TK_PAGE_DOWN,
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

// `fds` fills out[] with up to max descriptors to wait on alongside stdin and
// returns how many it wrote; `ready` runs whenever any of them wakes.
#define TTY_WATCH_MAX 8
void tty_watch(int (*fds)(void *ud, int *out, int max), void (*ready)(void *ud),
               void *ud);

int  tty_is_raw(void);

int tty_cooked_termios(struct termios *out);

size_t tty_take_pending(void *buf, size_t max);

int tty_input_waiting(void);

unsigned tty_resize_epoch(void);

#define TTY_RESIZE_SETTLE_MS 250

#define TTY_MIN_COLUMNS 20

// The width the layout is built for, floored at TTY_MIN_COLUMNS.
int  tty_columns(void);

// The width the screen actually has, however narrow.
int  tty_screen_columns(void);

int  tty_rows(void);

#endif
