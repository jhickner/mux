#ifndef CHROME_H
#define CHROME_H

// The one painter for everything under the transcript: gap, pending side
// turns, sticky prompt, queued lines, spinner, input. Nothing else opens a
// block.

struct prompt;

void chrome_bind(struct prompt *p);

void chrome_paint(void);

// Repaints only the spinner row. Zero if the block cannot take a row swap.
int  chrome_paint_spin(void);

void chrome_clear(void);

// Everything above the input becomes transcript; the input goes. For ctrl-D.
void chrome_keep_above(void);

// Screen rows the chrome has not spent yet.
int  chrome_rows_left(void);

// A modal owns the whole stack while set. NULL hands it back.
typedef void (*chrome_modal_fn)(void *ud);
void chrome_modal(chrome_modal_fn fn, void *ud);

#endif
