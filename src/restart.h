#ifndef RESTART_H
#define RESTART_H

struct session;

void restart_arm(int safe_mode);

void restart_shield_thread(void);

void restart_request(void);

int restart_wanted(void);

int restart_exec(struct session *s);

#endif
