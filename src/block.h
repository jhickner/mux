#ifndef BLOCK_H
#define BLOCK_H

#include <stddef.h>

// The chrome block that sits under the transcript. Every paint lands on
// absolute screen rows and every erase names the rows it wrote, so a reflow
// can move the block but can never leave a copy behind: the row the transcript
// writes to is tracked here, and a resize re-anchors it against the terminal
// instead of against a model of what the last paint left on screen.
//
// One rule holds the whole thing up — a painted block is never on screen when
// the transcript writes — and block_before_output() is the only thing that
// enforces it, from inside ui_putn(). Callers that print do not have to erase
// first, and cannot leave a block stranded by forgetting to; the ones that
// clear explicitly do it because they are handing the terminal to something
// else. tools/blocktest.c asserts the rule against a model of the screen.

void block_begin(void);
void block_end(int caret_row, int caret_col);

int  block_have(void);
void block_row_begin(int row);
void block_row_end(void);

void block_clear(void);
void block_keep(int rows);

// The terminal went somewhere this module cannot follow: a child had it, or the
// screen was cleared out from under it.
void block_forget(void);
void block_cleared(void);

int      block_out_row(void);
unsigned block_scrolls(void);

void block_before_output(void);
void block_wrote(const char *s, size_t n);
void block_newline(void);

#endif
