/* Slash commands: the registry the input dropdown completes against, and the
 * dispatcher that runs them. */
#ifndef CMD_H
#define CMD_H

#include "vendor/repl.h"

struct session;

extern const ReplCommand CMD_TABLE[];
extern const int         CMD_COUNT;

enum cmd_result {
    CMD_NOT_A_COMMAND, /* ordinary prompt text; send it to the model */
    CMD_HANDLED,
    CMD_QUIT,
};

enum cmd_result cmd_dispatch(struct session *s, const char *line);

/* Whether `line` is one of the commands safe to run with a turn in flight.
 * Asked before echoing, so a line destined for the queue is not shown twice. */
int cmd_is_live(const char *line);

/* Run such a command. The caller owns pausing the display around it. */
void cmd_dispatch_live(struct session *s, const char *line);

/* The /resume picker, also used for --resume at startup. Prints the identity
 * row itself on success. Returns 1 when a conversation was adopted. Requires
 * raw mode. */
int cmd_resume(struct session *s);

#endif /* CMD_H */
