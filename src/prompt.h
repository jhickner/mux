
#ifndef PROMPT_H
#define PROMPT_H

#include "tty.h"
#include "vendor/repl.h"

struct prompt;

struct prompt *prompt_new(const ReplCommand *commands, int command_count);
void           prompt_free(struct prompt *p);

void prompt_history_open(struct prompt *p, const char *path);

void prompt_file_completion(struct prompt *p, const char *root);

char *prompt_read(struct prompt *p);

int  prompt_live_key(void *ud, tty_event *ev);
void prompt_live_paint(void *ud, int *rows, int *caret_row, int *caret_col);

int  prompt_live_offset(void *ud);

typedef int (*prompt_hud_fn)(void *ud, int cols);
void prompt_set_hud(struct prompt *p, prompt_hud_fn fn, void *ud);

void prompt_set_replay(struct prompt *p, void (*fn)(void *ud), void *ud);

typedef int (*prompt_live_fn)(void *ud, const char *line);
void prompt_set_live_command(struct prompt *p, prompt_live_fn fn, void *ud);

/* render() consumes a turn the agent ran on its own; busy() answers whether the
 * agent still has work running with no turn in flight, so the sticky prompt can
 * keep showing the send as unfinished while background workers report in. */
void prompt_set_idle(struct prompt *p, int (*fd)(void *ud), int (*render)(void *ud),
                     int (*busy)(void *ud), void *ud);

char *prompt_take_queued(struct prompt *p);

void prompt_echo_message(const char *text);

#endif
