
#ifndef CMD_H
#define CMD_H

#include "vendor/repl.h"

struct session;

extern const ReplCommand CMD_TABLE[];
extern const int         CMD_COUNT;

enum cmd_result {
    CMD_NOT_A_COMMAND,
    CMD_HANDLED,
    CMD_QUIT,
};

enum cmd_result cmd_dispatch(struct session *s, const char *line);

int cmd_is_live(const char *line);

// The command puts its own line in the transcript, so echoing it the ordinary
// way would print it twice.
int cmd_self_echoes(const char *line);

void cmd_dispatch_live(struct session *s, const char *line);

// Runs the commands cmd_dispatch_live() held back because a turn was running.
void cmd_run_deferred(struct session *s);

int cmd_resume(struct session *s);

#endif
