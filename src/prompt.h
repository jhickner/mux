/* The input block: a repl.h line editor painted into normal scrollback (no
 * alternate screen), repainted in place above the terminal cursor. */
#ifndef PROMPT_H
#define PROMPT_H

#include "tty.h"
#include "vendor/repl.h"

struct prompt;

/* `commands` is borrowed and must outlive the prompt. */
struct prompt *prompt_new(const ReplCommand *commands, int command_count);
void           prompt_free(struct prompt *p);

/* Load history from `path` and append submitted lines to it. */
void prompt_history_open(struct prompt *p, const char *path);

/* Read one message. Returns malloc'd text (possibly multi-line), or NULL when
 * the user quits with Ctrl-D or EOF. The submitted text is left in scrollback
 * as a "▌ " block. Anything typed but not submitted during the previous turn is
 * still in the editor. */
char *prompt_read(struct prompt *p);

/* The same editor, kept live under the spinner while a turn runs (see
 * status_set_below). Keys go to it as usual and each submitted line joins the
 * queue instead of being returned; Escape, Ctrl-C and Ctrl-D on an empty line
 * ask to interrupt the turn, which is what a nonzero return means. */
int  prompt_live_key(void *ud, tty_event *ev);
void prompt_live_paint(void *ud, int *rows, int *caret_row, int *caret_col);

/* The oldest message queued during a turn, or NULL when none is waiting. The
 * caller owns it and is responsible for echoing it. */
char *prompt_take_queued(struct prompt *p);

/* Print `text` as the "▌ " user block without reading anything — for replaying
 * a message the caller supplied. */
void prompt_echo_message(const char *text);

#endif /* PROMPT_H */
