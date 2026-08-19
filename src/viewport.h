#ifndef VIEWPORT_H
#define VIEWPORT_H

#include <stddef.h>

// The screen mux owns. Everything the transcript prints is kept here as rows
// rather than handed to the terminal's scrollback, and every frame is painted
// whole: the transcript slice that is scrolled to, then the chrome under it.
//
// The point is what stops being a question. Nothing asks the terminal where
// the cursor is, nothing predicts how a resize reflowed the screen, and no
// paint has to be undone before the next one — a resize just recomputes the
// slice and paints it again.

int  viewport_active(void);

void viewport_begin(void);
void viewport_end(void);

// Transcript output, split into rows here. Escapes ride along inside a row and
// are re-emitted with it; they take no cells.
void viewport_write(const char *s, size_t n);

// Hand the terminal to a child and take it back. Suspend leaves the alt screen
// so the child gets a normal one; resume repaints from scratch.
void viewport_suspend(void);
void viewport_resume(void);

void viewport_paint(void);
void viewport_touch(void);

// The chrome under the transcript, as rows. Caret is a row within them and a
// column within that row; a negative column hides the cursor.
void viewport_chrome(char **rows, int n, int caret_row, int caret_col);
void viewport_chrome_clear(void);

// Replace one chrome row in place.
void viewport_chrome_row(int at, const char *s);

// The first `keep` chrome rows stop being chrome and become transcript.
void viewport_chrome_keep(int keep);

// Drop the last transcript row, so a caller that repaints a row it just wrote
// can replace it instead of stacking copies.
void viewport_drop_row(void);

void viewport_clear(void);

// Rows ever written, and the index of the first one on screen. A caller that
// wants to know whether something it wrote is still visible compares the row
// index it kept against the first visible one, which is exact.
int  viewport_rows(void);
int  viewport_first_visible(void);

// Write the transcript to the normal screen, so leaving mux leaves the
// conversation in the terminal's own scrollback.
void viewport_dump(void);

void viewport_scroll(int delta);
void viewport_scroll_end(void);
int  viewport_scrolled(void);

#endif
