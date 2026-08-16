
#ifndef TITLE_H
#define TITLE_H

#include <stddef.h>

int title_lookup(const char *id, char *out, size_t size);

void title_request(const char *id, const char *backend, const char *model,
                   const char *cwd, const char *prompt, const char *reply);

#endif
