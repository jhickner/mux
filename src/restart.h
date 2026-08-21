#ifndef RESTART_H
#define RESTART_H

struct session;

void restart_arm(int safe_mode);

void restart_shield_thread(void);

// An argv entry to carry into the new build, on top of what the session says.
void restart_flag(const char *flag);

void restart_request(void);

int restart_wanted(void);

int restart_exec(struct session *s);

#endif
