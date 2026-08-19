
#ifndef STATUS_H
#define STATUS_H

#include "ui.h"

void   status_begin(void);
void   status_end(void);

typedef void (*status_paint_fn)(void *ud, int *rows, int *caret_row, int *caret_col);

void   status_set_below(status_paint_fn paint, void *ud);

typedef void (*status_above_fn)(void *ud);

void   status_set_above(status_above_fn paint, void *ud);

// Screen rows the chrome painted so far may still spend. Callers that paint
// above the spinner must stop once this reaches zero.
int    status_rows_left(void);

void   status_set_word(const char *text);

void   status_set_note(const char *text);

void   status_tick(void);

void   status_touch(void);

void   status_pause(void);
void   status_resume(void);

void   status_gap(int on);

#define STICKY_LINES 3

void   status_sticky_set(int on);
int    status_sticky_enabled(void);

void   status_sticky_prompt(const char *text);

int    status_sticky_rows(void);

const char *status_sticky_offscreen(void);

void   status_sticky_erased(void);

double status_elapsed(void);

#endif
