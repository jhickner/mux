#ifndef HANDOFF_H
#define HANDOFF_H

#include <stddef.h>

// Moving a conversation from the window holding it into this one. The agent
// process is not carried across: the window that had it lets it go, and this
// one resumes the same conversation, the way /restart already does within a
// process.

// Asked for: a request naming a session this process holds is waiting.
int handoff_wanted(void);

// Takes the request, filling in the session id that was asked for.
int handoff_take_request(char *id, size_t size);

// Nothing here holds that session; the asker should stop waiting.
void handoff_refuse(const char *id);

// Where the screen for `id` should be dumped, and the announcement that the
// conversation is free once it has been. Published last: the asker treats it
// as the moment the other window let go.
int handoff_screen_path(const char *id, char *out, size_t size);
int handoff_publish(const char *id);

// Asks the window at `pid` for `id` and waits for it to let go. Fills `screen`
// with the dump to restore, which the caller unlinks once it has it. `tick`,
// where given, is called every poll with how long the wait has run, so a
// window that is slow to answer can be said so.
int handoff_ask(long pid, const char *id, char *screen, size_t size,
                void (*tick)(int waited_ms, void *ud), void *ud);

#endif
