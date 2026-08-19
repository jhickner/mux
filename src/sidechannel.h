
#ifndef SIDECHANNEL_H
#define SIDECHANNEL_H

struct session;

// Runs `prompt` as a one-turn fork of the session in a background mux, so it
// answers alongside whatever the main conversation is doing. Returns nonzero
// once the child is running.
// `prompt` is what the fork is asked; `label` is what the transcript shows,
// which is the line as it was typed rather than the part that was sent.
int sidechannel_start(const struct session *s, const char *prompt, const char *label);

// Descriptors to wait on while a side turn is in flight.
int sidechannel_fds(int *out, int max);

// Drains finished side turns and prints their replies. Cheap when idle.
void sidechannel_poll(void);

// Advances the spinner on every question still waiting for its answer.
void sidechannel_tick(void);

// Questions still waiting are painted as chrome, one row each, pinned under
// the sticky prompt rather than scrolling away in the transcript.
int  sidechannel_rows(void);
void sidechannel_paint(int budget);

int sidechannel_busy(void);

void sidechannel_close_all(void);

#endif
