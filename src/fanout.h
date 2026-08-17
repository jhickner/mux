
#ifndef FANOUT_H
#define FANOUT_H

struct session;

/* Asks every backend in the roster the same thing, each in its own throwaway
   conversation, and shows them side by side as they work: one column each,
   growing until the whole board scrolls. A terminal too narrow for columns
   falls back to printing the replies one backend after another. Returns the
   number that answered. */
int fanout_run(struct session *s, const char *prompt);

#endif
