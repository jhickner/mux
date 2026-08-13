/* The activity indicator shown while a turn is in flight: a spinner row, plus
 * whatever rows the live input block adds below it. It owns the bottom of the
 * terminal, so anything printed during the turn must be bracketed by
 * status_pause()/status_resume() to lift the block out of the way. */
#ifndef STATUS_H
#define STATUS_H

#include <stddef.h>

void   status_begin(void);
void   status_end(void);

/* Rows painted directly beneath the spinner — the prompt that stays live while
 * a turn runs. `paint` draws from column 0 of the row below the spinner, leaves
 * the terminal cursor at the end of its last row, and reports how many rows it
 * drew and where the caret belongs within them. NULL for no block. */
typedef void (*status_paint_fn)(void *ud, int *rows, int *caret_row, int *caret_col);

/* Asked, when the block is about to be erased, how many rows now separate its
 * first row from the caret: a resize rewraps the block on screen, so this can
 * be more than the last paint reported. NULL to trust the paint. */
typedef int  (*status_offset_fn)(void *ud);

void   status_set_below(status_paint_fn paint, status_offset_fn offset, void *ud);

/* Trailing text for the spinner row — the conversation's name. Shown only when
 * the row is wide enough for all of it. NULL or "" removes it. */
void   status_set_note(const char *text);

/* Redraw the spinner and elapsed time; cheap enough to call on every poll. */
void   status_tick(void);

/* Erase the block before printing scrollback content, then bring it back. */
void   status_pause(void);
void   status_resume(void);

/* Carry a blank row above the spinner, so it does not sit flush against the
 * tool rows printed just before it. The row belongs to the block and is erased
 * with it, leaving the spacing that survives in scrollback to the caller. */
void   status_gap(int on);

/* Seconds since status_begin(). */
double status_elapsed(void);

#endif /* STATUS_H */
