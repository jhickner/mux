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

// Everything printed between begin and end is one entry, and `render` can draw
// it again at any width. A resize re-renders it rather than re-wrapping rows
// that were laid out for a width the screen no longer has, so gutters, indents
// and tables come back right instead of being chopped.
//
// Output that is not wrapped this way is still kept, and still survives a
// resize; it just soft-wraps rather than re-flowing.
typedef void (*viewport_render_fn)(void *ud, int cols);

// Returns the mark of the entry it opens. Taking it beforehand would name
// the wrong entry: opening one flushes whatever unwrapped output was still
// in hand, and that becomes an entry of its own.
unsigned viewport_item_begin(viewport_render_fn render, void *ud, void (*free_ud)(void *));
void viewport_item_end(void);

// Hand the terminal to a child and take it back. Suspend leaves the alt screen
// so the child gets a normal one; resume repaints from scratch.
void viewport_suspend(void);
void viewport_resume(void);

void viewport_paint(void);

// The screen is no longer what the last paint left: something else wrote to
// it. The next paint sends every row instead of only what changed.
void viewport_forget(void);
void viewport_touch(void);

// The chrome under the transcript, as rows. Caret is a row within them and a
// column within that row; a negative column hides the cursor.
void viewport_chrome(char **rows, int n, int caret_row, int caret_col);
void viewport_chrome_clear(void);

// Replace one chrome row in place.
void viewport_chrome_row(int at, const char *s);

// The first `keep` chrome rows stop being chrome and become transcript.
void viewport_chrome_keep(int keep);

// Whether the transcript already ends in a blank row, so a caller that wants
// one above what it is about to print can tell whether it is owed one.
int viewport_ends_blank(void);

void viewport_clear(void);

// The id the next entry will get, and whether the entry with that id is still
// on screen. A caller keeps the mark it took before printing and asks later;
// the answer is exact, with no running count to drift.
unsigned viewport_mark(void);
int      viewport_visible(unsigned mark);

// The payload of a kept entry, or NULL once it has been dropped — evicted with
// the oldest entries, or cleared. A caller that wants to keep changing what it
// printed holds the mark rather than the pointer, so nothing it holds can
// outlive the entry.
void *viewport_item_data(unsigned mark);

// The entry's payload changed: draw it again.
void  viewport_item_update(unsigned mark);

void viewport_scroll(int delta);
void viewport_scroll_end(void);
int  viewport_scrolled(void);

#endif
