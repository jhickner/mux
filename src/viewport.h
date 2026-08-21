#ifndef VIEWPORT_H
#define VIEWPORT_H

#include <stddef.h>

// The screen mux owns. The transcript is kept here as rows rather than handed
// to the terminal's scrollback, and every frame is painted whole.

int  viewport_active(void);

void viewport_begin(void);
void viewport_end(void);

// Restarting into a new binary: the alt screen is handed over rather than torn
// down, and the entries travel through a file.
void viewport_handoff(void);
void viewport_inherit(void);
int  viewport_dump(const char *path);

// Transcript output, split into rows. Escapes ride inside a row, taking no cells.
void viewport_write(const char *s, size_t n);

// Everything printed between begin and end is one entry, which `render` draws
// again at any width. Output not wrapped this way soft-wraps on a resize.
typedef void (*viewport_render_fn)(void *ud, int cols);

// What an entry is, declared the same way whatever it holds: how it draws,
// whether it may be drawn again, and the blank rows it wants around it.
//
// `reflow` asks for render() again when the width changes; without it the rows
// stand as they were first printed. An entry never draws its own padding: the
// seam between two entries takes the larger of what the two sides ask for, and
// blank rows already there count toward it, so a run of padded entries is
// separated by one row rather than two or none.
struct viewport_entry {
    viewport_render_fn render;      /* NULL: nothing to draw it again with */
    void              *ud;
    void             (*free_ud)(void *);
    int                reflow;
    int                pad_before;
    int                pad_after;
};

// The common case, written out where it is used: printed rows that nothing
// redraws, asking for the blank rows around them.
#define VIEWPORT_ROWS(before, after) \
    (&(struct viewport_entry){.pad_before = (before), .pad_after = (after)})

// Returns the mark of the entry it opens. Taking it beforehand names the wrong
// entry: opening one first closes any unwrapped output as an entry of its own.
// Zero when nothing was opened: output going somewhere other than the
// transcript, or no viewport at all.
unsigned viewport_item_begin(const struct viewport_entry *e);
void viewport_item_end(void);

// Hand the terminal to a child and take it back.
void viewport_suspend(void);
void viewport_resume(void);

void viewport_paint(void);

// Something else wrote to the screen: send every row on the next paint.
void viewport_forget(void);
void viewport_touch(void);

// The chrome under the transcript. Caret is a row within it and a column
// within that row; a negative column hides the cursor.
void viewport_chrome(char **rows, int n, int caret_row, int caret_col);
void viewport_chrome_clear(void);

void viewport_chrome_row(int at, const char *s);

// The first `keep` chrome rows stop being chrome and become transcript.
void viewport_chrome_keep(int keep);

int viewport_ends_blank(void);

void viewport_clear(void);

// The id the next entry will get, and whether that entry is still on screen.
unsigned viewport_mark(void);
int      viewport_visible(unsigned mark);

// The payload of a kept entry, or NULL once it has been dropped. Holding the
// mark rather than the pointer keeps nothing alive past the entry.
void *viewport_item_data(unsigned mark);

void  viewport_item_update(unsigned mark);

// An entry that survives a restart. `kind` names the loader that rebuilds it
// and `encode` writes its payload as JSON, called at dump time so an entry
// still being amended travels as it last stood.
typedef char *(*viewport_encode_fn)(void *ud);
void viewport_item_persist(unsigned mark, const char *kind, viewport_encode_fn encode);

// A screen held aside. A window with several sessions gives each one of these
// and swaps them whole, so a session that is not on screen still has somewhere
// to draw and comes back exactly as it was left.
struct viewport_state;

struct viewport_state *viewport_state_new(void);
void viewport_state_free(struct viewport_state *st);

// Nothing reaches the terminal while a screen other than the one showing is in
// place: a background session's own drawing must not paint over the window.
void viewport_hold(int on);

// The screen goes into `st` and what is left is empty; `adopt` puts one back.
void viewport_stash(struct viewport_state *st);
void viewport_adopt(struct viewport_state *st);

void viewport_scroll(int delta);
void viewport_scroll_end(void);
int  viewport_scrolled(void);

#endif
