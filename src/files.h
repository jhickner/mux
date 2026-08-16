
#ifndef FILES_H
#define FILES_H

#include "vendor/repl.h"

int files_complete(void *ctx, const char *token, ReplCandidate *out, int max);

void files_forget(void);

#endif
