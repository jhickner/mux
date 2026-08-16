
#ifndef SESSIONLIST_H
#define SESSIONLIST_H

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

#endif
