#ifndef CHROME_H
#define CHROME_H

// The one painter for everything under the transcript.
//
// The stack is composed here — gap, sticky prompt, pending side turns, queued
// lines, spinner, input — in one order, in one function. Nothing else opens a
// block.
//
// It used to be two painters: status.c composed the live turn's chrome and
// called into prompt.c for part of it, while prompt.c composed the idle
// prompt's chrome and guessed which half it was in from a flag. Each knew
// about some of the stack, so a section could be claimed by both and drawn
// twice, and adding one meant deciding which painter it belonged to.

struct prompt;

void chrome_bind(struct prompt *p);

void chrome_paint(void);

// Only the spinner row changed, which is most frames while a turn runs.
// Returns zero if the block is not in a state where one row can be replaced.
int  chrome_paint_spin(void);

void chrome_clear(void);

// What was painted above the input stops being chrome and stays in the
// transcript; the input itself goes. What ctrl-D leaves behind.
void chrome_keep_above(void);

// Screen rows the chrome has not spent yet.
int  chrome_rows_left(void);

// A modal owns the whole stack while it is set: a y/n question, a picker.
// Passing NULL hands it back.
typedef void (*chrome_modal_fn)(void *ud);
void chrome_modal(chrome_modal_fn fn, void *ud);

#endif
