/* The one-line activity indicator shown while a turn is in flight. It owns the
 * last line of the terminal: anything printed during the turn must be bracketed
 * by status_pause()/status_resume() so the line is lifted out of the way. */
#ifndef STATUS_H
#define STATUS_H

#include <stddef.h>

void   status_begin(void);
void   status_end(void);

/* Redraw the spinner and elapsed time; cheap enough to call on every poll. */
void   status_tick(void);

/* Erase the line before printing scrollback content, then bring it back. */
void   status_pause(void);
void   status_resume(void);

/* Replace the trailing activity label (e.g. "Bash(git status)"). NULL clears. */
void   status_activity(const char *label);

/* Seconds since status_begin(). */
double status_elapsed(void);

#endif /* STATUS_H */
