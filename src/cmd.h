
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

void cmd_dispatch_live(struct session *s, const char *line);

int cmd_resume(struct session *s);

#endif
