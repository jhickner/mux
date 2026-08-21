#ifndef CHROME_H
#define CHROME_H

// The one painter for everything under the transcript: gap, pending side
// turns, sticky prompt, queued lines, spinner, input. Nothing else opens a
// block.

struct prompt;

void chrome_bind(struct prompt *p);

// The row of tabs above everything else, when the window holds more than one
// session. Unset draws nothing.
void chrome_tabs(int (*rows)(void), void (*paint)(void));

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

// Whether one is up. Anything that would open a modal of its own asks first:
// a second one nested inside the first owns nothing.
int  chrome_modal_active(void);

// A modal cannot stand in the way of something the window has to answer — a
// request from elsewhere for one of its sessions, which is being waited on.
// The modals ask this whenever a read comes up empty, and cancel when it says
// so.
void chrome_modal_interrupt(int (*fn)(void));
int  chrome_modal_interrupted(void);

#endif
