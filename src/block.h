#ifndef BLOCK_H
#define BLOCK_H

#include <stddef.h>

// The chrome under the transcript: the spinner, the sticky prompt, the input.
// The painters render into a buffer, this splits it into rows, and the
// viewport paints them with everything else. Nothing here knows where the
// terminal's cursor is, because nothing needs to.

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

#endif
