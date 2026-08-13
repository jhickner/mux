/* Past conversations for this directory, read out of the CLI's own transcript
 * store (~/.claude/projects/<encoded cwd>/<session-id>.jsonl). */
#ifndef SESSIONLIST_H
#define SESSIONLIST_H

#include <time.h>

struct past_session {
    char   id[128];
    char   label[200]; /* the opening user message, flattened */
    char   when[32];   /* "4m ago", "3d ago" */
    time_t modified;
};

/* Fill *out with sessions for `cwd`, newest first (caller frees *out). Returns
 * the count, or 0 when there are none. `skip_id` may be NULL. */
int sessionlist_load(const char *cwd, const char *skip_id, struct past_session **out);

#endif /* SESSIONLIST_H */
