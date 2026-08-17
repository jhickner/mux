
#ifndef SIDECHANNEL_H
#define SIDECHANNEL_H

struct session;

// Runs `prompt` as a one-turn fork of the session in a background mux, so it
// answers alongside whatever the main conversation is doing. Returns nonzero
// once the child is running.
int sidechannel_start(const struct session *s, const char *prompt);

// Descriptors to wait on while a side turn is in flight.
int sidechannel_fds(int *out, int max);

// Drains finished side turns and prints their replies. Cheap when idle.
void sidechannel_poll(void);

int sidechannel_busy(void);

void sidechannel_close_all(void);

#endif
