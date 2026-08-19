
#ifndef STATUS_H
#define STATUS_H

#include "ui.h"

void   status_begin(void);
void   status_end(void);

// The sections chrome.c composes. Each draws itself and nothing else; where
// they sit in the stack is not their business.
void   status_paint_spin(void);
void   status_paint_sticky(int busy);
int    status_sticky_measure(int busy);

// A turn is running and its spinner belongs in the stack.
int    status_spinning(void);

int    status_gap_row(void);

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
