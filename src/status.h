
#ifndef STATUS_H
#define STATUS_H

#include <stddef.h>

void   status_begin(void);
void   status_end(void);

typedef void (*status_paint_fn)(void *ud, int *rows, int *caret_row, int *caret_col);

typedef int  (*status_offset_fn)(void *ud);

void   status_set_below(status_paint_fn paint, status_offset_fn offset, void *ud);

typedef int  (*status_hud_fn)(void *ud, int cols);
void   status_set_hud(status_hud_fn paint, void *ud);

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

const char *status_sticky_offscreen(void);

void   status_sticky_erased(void);

double status_elapsed(void);

#endif
