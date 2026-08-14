/* The activity indicator shown while a turn is in flight: a spinner row, the
 * optional floating prompt above it, plus whatever rows the live input block
 * adds below it. It owns the bottom of the
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

/* The chrome rows above the spinner — the same ones an idle prompt carries
 * above its caret, so they hold still as a turn starts and ends. Paints them at
 * `cols`, each ending in a newline and each exactly one physical row, and
 * returns the count. NULL for none. */
typedef int  (*status_hud_fn)(void *ud, int cols);
void   status_set_hud(status_hud_fn paint, void *ud);

/* Activity phrase after the elapsed time — "working", or "thinking with
 * medium effort". Shown only when the row is wide enough. NULL or ""
 * restores "working". */
void   status_set_word(const char *text);

/* Trailing text for the spinner row — the conversation's name. Shown only when
 * the row is wide enough for all of it. NULL or "" removes it. */
void   status_set_note(const char *text);

/* Redraw the spinner and elapsed time when something has changed, so a driver
 * polling its abort predicate tens of times a second costs nothing extra. */
void   status_tick(void);

/* Something the block draws has changed — typing edited the live prompt, say —
 * so the next tick has to repaint even if the spinner frame has not moved on. */
void   status_touch(void);

/* Erase the block before printing scrollback content, then bring it back. */
void   status_pause(void);
void   status_resume(void);

/* Carry a blank row above the spinner, so it does not sit flush against the
 * tool rows printed just before it. The row belongs to the block and is erased
 * with it, leaving the spacing that survives in scrollback to the caller. */
void   status_gap(int on);

/* Longest the floating prompt is drawn. A longer message is clipped and the
 * last row shown ends in an ellipsis. */
#define STICKY_LINES 3

/* The floating prompt: the message the turn is answering, carried at the top of
 * the block so it stays on screen however much output scrolls past. It appears
 * only once the turn's output has carried the echoed original off the top, so
 * the message is never shown twice. Off by default; the setting and `/sticky`
 * decide. */
void   status_sticky_set(int on);
int    status_sticky_enabled(void);

/* The message the next turn answers, set once its echo has been printed and
 * before the turn starts. NULL clears it. */
void   status_sticky_prompt(const char *text);

/* That message once the turn's output has carried its echo off the top of the
 * screen, so an idle prompt can go on showing it; NULL while the echo is still
 * up there, or when the feature is off. */
const char *status_sticky_offscreen(void);

/* The screen was wiped, which takes the echo with it however little had
 * scrolled. */
void   status_sticky_erased(void);

/* Seconds since status_begin(), or 0 if no turn is in flight. */
double status_elapsed(void);

#endif /* STATUS_H */
