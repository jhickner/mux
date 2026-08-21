
#ifndef SESSIONLIST_H
#define SESSIONLIST_H

#include <stddef.h>
#include <time.h>

struct past_session {
    char   id[128];
    char   label[200];
    char   when[32];
    time_t modified;
};

int sessionlist_available(const char *backend);

int sessionlist_load(const char *backend, const char *cwd, const char *skip_id,
                     struct past_session **out);

// Where the backend keeps this directory's conversations, so one of them can be
// found by id without reading the whole list.
int sessionlist_dir(const char *backend, const char *cwd, char *out, size_t size);

#endif
