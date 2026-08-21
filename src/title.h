
#ifndef TITLE_H
#define TITLE_H

#include <stddef.h>

int title_lookup(const char *id, char *out, size_t size);

// Names the session outright, without asking the model.
int title_set(const char *id, const char *name);

void title_request(const char *id, const char *backend, const char *model,
                   const char *cwd, const char *prompt, const char *reply);

#endif
