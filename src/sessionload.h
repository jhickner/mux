#ifndef SESSIONLOAD_H
#define SESSIONLOAD_H

#include <stddef.h>

// The conversation as it was, read back from the CLI's own transcript. A
// session opened in a window that never held it has no scrollback of its own,
// so this is where the history it should be showing comes from.

int sessionload_available(const char *backend);

// The file that holds the conversation, for the backends that keep one.
int sessionload_path(const char *backend, const char *cwd, const char *id,
                     char *out, size_t size);

// Draws the past turns into the screen in front. `thinking` says whether the
// model's reasoning is shown, as the session's own setting does. Returns the
// number of turns drawn, or 0 when there was nothing to read.
int sessionload_replay(const char *backend, const char *cwd, const char *id,
                       int thinking);

// The same for a session already open here, which knows all three.
struct session;
int sessionload_into(const struct session *s);

#endif
