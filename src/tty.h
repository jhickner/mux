/* Raw-mode terminal input: UTF-8 aware key decoding on a blocking reader. */
#ifndef TTY_H
#define TTY_H

#include <stdint.h>

typedef enum {
    TK_CHAR,       /* ev.cp holds a codepoint; control bytes arrive as cp < 0x20 */
    TK_TEXT,       /* ev.text holds a bracketed-paste body (heap, caller frees)  */
    TK_ENTER,
    TK_NEWLINE,    /* Ctrl-J or Alt-Enter: insert a literal newline              */
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

/* Enter raw mode and enable bracketed paste. Returns 0 on success, -1 when
 * stdin is not a terminal. Idempotent. */
int  tty_raw_begin(void);

/* Restore the entry terminal mode. Safe to call when not in raw mode, and safe
 * from an exit handler. */
void tty_raw_end(void);

/* Wait up to timeout_ms (<0 waits indefinitely) for one key. Returns 1 with *ev
 * filled, or 0 on timeout. */
int  tty_read(tty_event *ev, int timeout_ms);

/* Nonzero while the terminal is in raw mode. */
int  tty_is_raw(void);

/* Bumped for every size change the terminal reports, whether or not the event
 * is ever read, so a caller that only repaints can tell the window moved. */
unsigned tty_resize_epoch(void);

/* A drag or a zoom delivers a burst of size changes. Callers that redraw on
 * resize coalesce the burst by waiting this long for the size to go quiet.
 *
 * Sized by measurement, not feel: a multiplexer reports the new width before it
 * has finished rewrapping the rows already on screen, and redrawing inside that
 * gap strands them. Stranding was still reproducible at 120ms and stopped at
 * 200ms, so this leaves margin over the point where it cleared. */
#define TTY_RESIZE_SETTLE_MS 250

/* Current terminal width in columns, floored at 20. */
int  tty_columns(void);

/* Current terminal height in rows, floored at 3. */
int  tty_rows(void);

/* Ask the terminal where its cursor is. Any keystrokes that arrive around the
 * reply stay queued for tty_read(). Returns 1 on success. */
int  tty_cursor_position(int *row, int *column);

#endif /* TTY_H */
