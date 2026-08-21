
#ifndef STATUS_H
#define STATUS_H

#include "ui.h"

// How long a spinner frame lasts. Shared, because more than one thing spins
// and they are next to each other: a spinner advanced once per call rather
// than once per frame runs at whatever rate its caller happens to poll at,
// which is only visibly wrong beside one that does not.
#define SPIN_FRAME_MS 90

// Advances `*frame` if a frame's worth of time has passed since `*at`, and
// says whether it moved. Every spinner goes through this, so none of them can
// run at the rate of whatever loop happens to be asking.
int    spin_advance(int *frame, double *at);

void   status_begin(void);
void   status_begin_at(double elapsed);
void   status_end(void);

// The sections chrome.c composes. Each draws itself and nothing else; where
// they sit in the stack is not their business.
void   status_paint_spin(void);
void   status_paint_sticky(void);
int    status_sticky_measure(void);

// A turn is running and its spinner belongs in the stack.
int    status_spinning(void);


void   status_set_word(const char *text);

void   status_set_note(const char *text);

void   status_tick(void);

void   status_touch(void);

void   status_pause(void);
void   status_resume(void);


#define STICKY_LINES 3

void   status_sticky_set(int on);
int    status_sticky_enabled(void);

void   status_sticky_prompt(const char *text);

int    status_sticky_rows(void);

const char *status_sticky_offscreen(void);

void   status_sticky_busy(int on);

void   status_sticky_erased(void);

double status_elapsed(void);

#endif
