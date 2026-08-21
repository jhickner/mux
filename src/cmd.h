
#ifndef CMD_H
#define CMD_H

#include "vendor/repl.h"

struct session;
struct pick_item;

// The model and effort lists the pickers offer for a backend. Never empty:
// a backend with no list of its own gets a lone "default".
const struct pick_item *cmd_model_choices(const char *backend, int *count);
const struct pick_item *cmd_effort_choices(const char *backend, int *count);

// The commands offered by tab-completion, minus the hidden ones.
const ReplCommand *cmd_completions(int *count);

enum cmd_result {
    CMD_NOT_A_COMMAND,
    CMD_HANDLED,
    CMD_QUIT,
};

enum cmd_result cmd_dispatch(struct session *s, const char *line);

// Whether a line typed behind a running turn belongs to cmd_dispatch_live():
// every command except the ones that end the window.
int cmd_runs_mid_turn(const char *line);

// Whether the line names a command at all, and whether that command is one
// that ends the window.
int cmd_is_command(const char *line);
int cmd_is_quit(const char *line);

// The command puts its own line in the transcript, so echoing it the ordinary
// way would print it twice.
int cmd_self_echoes(const char *line);

void cmd_dispatch_live(struct session *s, const char *line);

// Runs the commands cmd_dispatch_live() held back because a turn was running.
void cmd_run_deferred(struct session *s);

// Discards the ones held back for `s`, which is about to be freed.
void cmd_forget_session(struct session *s);

int cmd_resume(struct session *s);

#endif
