/* Completion candidates for an "@" token: the project's files and directories,
 * fuzzy-matched against what has been typed after the '@'. */
#ifndef FILES_H
#define FILES_H

#include "vendor/repl.h"

/* A ReplCompleter. `ctx` is the project root to index (a borrowed path, "." when
 * NULL); `token` is the '@'-stripped partial path. Candidates are bare paths —
 * the '@' already in the line is left in place when one is accepted. */
int files_complete(void *ctx, const char *token, ReplCandidate *out, int max);

/* Release the cached index. */
void files_forget(void);

#endif /* FILES_H */
