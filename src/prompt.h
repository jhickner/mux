
#ifndef PROMPT_H
#define PROMPT_H

#include "vendor/cJSON.h"

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

// The keys the prompt's input path implements, with what they do. Lives beside
// that code so a binding and its documentation cannot drift apart; /help just
// renders this. Prompt keys only — the shell escape and the prose about skills
// belong to their own owners.
struct prompt_key {
    const char *key;
    const char *desc;
};

const struct prompt_key *prompt_shortcuts(int *count);

int  prompt_live_key(void *ud, tty_event *ev);
// The sections chrome.c composes.
int  prompt_input_rows(struct prompt *p, int cols);
void prompt_paint_input(struct prompt *p, int rows, int *caret_row, int *caret_col);
// A queued line is a reminder of what is waiting, not the thing itself, so a
// long one is cut short rather than allowed to push everything else off the
// chrome. Consecutive ones are separated by a blank row, so several read as a
// list rather than as one run-on block.
#define QUEUED_LINES 2

int  prompt_queued_rows(struct prompt *p, int cols);
void prompt_paint_queued(struct prompt *p, int room);
int  prompt_busy(struct prompt *p);



void prompt_set_replay(struct prompt *p, void (*fn)(void *ud), void *ud);

// Enter on an empty line.
void prompt_set_blank(struct prompt *p, void (*fn)(void *ud), void *ud);

typedef int (*prompt_live_fn)(void *ud, const char *line);
void prompt_set_live_command(struct prompt *p, prompt_live_fn fn, void *ud);

// Whether a submitted line should be echoed into the transcript. Some commands
// put their own line there and would otherwise be printed twice. Unset means
// everything is echoed.
void prompt_set_echo_filter(struct prompt *p, int (*fn)(void *ud, const char *line),
                            void *ud);
int  prompt_echoes(struct prompt *p, const char *line);

// Something that cannot wait for a quiet prompt the way a restart can: another
// window is asking for one of this window's sessions.
void prompt_set_takeover(struct prompt *p, int (*pending)(void *ud), void (*run)(void *ud),
                         void *ud);

// Escape or ctrl-C on an empty line. Nonzero means it was taken — a turn was
// running and has been asked to stop.
void prompt_set_cancel(struct prompt *p, int (*fn)(void *ud), void *ud);

// Left arrow on an empty line.
void prompt_set_switcher(struct prompt *p, void (*fn)(void *ud), void *ud);

// Ctrl-B: another session in this window — an idle tab if there is one, else
// a new one like the current session.
void prompt_set_another(struct prompt *p, void (*fn)(void *ud), void *ud);

// Ctrl-T: a shell split beside this one. `quiet` while a turn is streaming.
void prompt_set_split(struct prompt *p, void (*fn)(void *ud, int quiet), void *ud);

// Ends the read in progress with no line, as ctrl-D does.
void prompt_stop(struct prompt *p);

void prompt_set_restart(struct prompt *p, int (*pending)(void *ud), int (*run)(void *ud),
                        void *ud);

// Take a pending restart if nothing is typed or queued. Called where a turn
// ends; the prompt loop cannot be relied on to be re-entered before the next.
void prompt_restart_check(struct prompt *p);

// A thing that animates while the prompt is idle. `busy` says whether anything
// is running; while it is, the read waits in frames instead of indefinitely so
// `tick` can advance it.
void prompt_set_animate(struct prompt *p, int (*busy)(void *ud), void (*tick)(void *ud),
                        void *ud);

void prompt_set_idle(struct prompt *p, int (*fds)(void *ud, int *out, int max),
                     int (*render)(void *ud), int (*busy)(void *ud), void *ud);

char *prompt_take_queued(struct prompt *p);

// A source of lines that were never typed: the Telegram bridge hands the prompt
// what the chat sent, and it submits as if it had been. Polled whenever the
// read is woken, so the source must also wake it (tty_wake).
void prompt_set_external(struct prompt *p, char *(*fn)(void *ud), void *ud);

// Whether the line prompt_read just returned came from that source.
int  prompt_line_was_external(struct prompt *p);

void prompt_echo_message(const char *text);

// Carried across a restart.
#define PROMPT_ECHO_KIND "echo"
void prompt_echo_load(const cJSON *st);

#endif
