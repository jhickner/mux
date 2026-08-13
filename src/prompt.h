/* The input block: a repl.h line editor painted into normal scrollback (no
 * alternate screen), repainted in place above the terminal cursor. */
#ifndef PROMPT_H
#define PROMPT_H

#include "vendor/repl.h"

struct prompt;

/* `commands` is borrowed and must outlive the prompt. */
struct prompt *prompt_new(const ReplCommand *commands, int command_count);
void           prompt_free(struct prompt *p);

/* Load history from `path` and append submitted lines to it. */
void prompt_history_open(struct prompt *p, const char *path);

/* Read one message. Returns malloc'd text (possibly multi-line), or NULL when
 * the user quits with Ctrl-D or EOF. The submitted text is left in scrollback
 * as a "▌ " block. */
char *prompt_read(struct prompt *p);

/* Print `text` as the "▌ " user block without reading anything — for replaying
 * a message the caller supplied. */
void prompt_echo_message(const char *text);

#endif /* PROMPT_H */
