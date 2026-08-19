
#ifndef PROMPT_H
#define PROMPT_H

#include "tty.h"
#include "ui.h"
#include "vendor/repl.h"

struct prompt;

struct prompt *prompt_new(const ReplCommand *commands, int command_count);
void           prompt_free(struct prompt *p);

void prompt_history_open(struct prompt *p, const char *path);

void prompt_file_completion(struct prompt *p, const char *root);

void prompt_rehome(const char *root);

char *prompt_read(struct prompt *p);

int  prompt_live_key(void *ud, tty_event *ev);
// The sections chrome.c composes.
int  prompt_input_rows(struct prompt *p, int cols);
void prompt_paint_input(struct prompt *p, int rows, int *caret_row, int *caret_col);
int  prompt_queued_rows(struct prompt *p, int cols);
void prompt_paint_queued(struct prompt *p, int room);
int  prompt_busy(struct prompt *p);



void prompt_set_replay(struct prompt *p, void (*fn)(void *ud), void *ud);

typedef int (*prompt_live_fn)(void *ud, const char *line);
void prompt_set_live_command(struct prompt *p, prompt_live_fn fn, void *ud);

// Whether a submitted line should be echoed into the transcript. Some commands
// put their own line there and would otherwise be printed twice. Unset means
// everything is echoed.
void prompt_set_echo_filter(struct prompt *p, int (*fn)(void *ud, const char *line),
                            void *ud);
int  prompt_echoes(struct prompt *p, const char *line);

void prompt_set_restart(struct prompt *p, int (*pending)(void *ud), int (*run)(void *ud),
                        void *ud);

// A thing that animates while the prompt is idle. `busy` says whether anything
// is running; while it is, the read waits in frames instead of indefinitely so
// `tick` can advance it.
void prompt_set_animate(struct prompt *p, int (*busy)(void *ud), void (*tick)(void *ud),
                        void *ud);

void prompt_set_idle(struct prompt *p, int (*fds)(void *ud, int *out, int max),
                     int (*render)(void *ud), int (*busy)(void *ud), void *ud);

char *prompt_take_queued(struct prompt *p);

void prompt_echo_message(const char *text);

#endif
