#ifndef CHATTABS_H
#define CHATTABS_H

// The conversations the chat holds when there is no terminal. A tab here is a
// session and nothing else — no screen, no prompt, no tab strip — because the
// process this runs in has nowhere to draw. The chat points at one of them at
// a time; the rest keep working, and say so when their turn ends.
//
// This is the headless twin of workspace.c, which is the same idea wrapped
// around a terminal's viewport.

#define CHATTABS_MAX 8

struct session;

// The session already started is the first tab.
int  chattabs_begin(struct session *first);
void chattabs_end(void);

// Called on a session this module has just created, before it is started, so
// the bridge can put its own settings on it. The one on the first tab has been
// prepared by whoever made it.
void chattabs_on_open(void (*fn)(struct session *s));

// Called when a turn ends on a tab that is not the one the chat is pointed at.
void chattabs_on_background(void (*fn)(struct session *s, int index));

struct session *chattabs_current(void);
struct session *chattabs_at(int index);
int  chattabs_count(void);
int  chattabs_index(void);
int  chattabs_index_of(const struct session *s);

// Starts a session with the current tab's backend and settings, in `cwd` (NULL
// for where the current one is), and points the chat at it. `id` resumes a
// past conversation. Returns the new index, or -1.
int  chattabs_open(const char *cwd, const char *id);

// Points the chat at another tab. Nonzero when the index named one.
int  chattabs_show(int index);

// The tab this conversation is already open in, by the backend's id for it.
int  chattabs_find_id(const char *id);

// Frees the session and drops the tab. Returns what is left, so zero is the
// caller's cue that there is nothing to talk to.
int  chattabs_close(int index);

// A turn ended on this tab while the chat was elsewhere and nobody has looked
// since.
int  chattabs_unseen(int index);

// Idle work for every tab. This is what advances a turn running behind the
// one the chat is pointed at.
int  chattabs_fds(int *out, int max);
void chattabs_pump(void);

// Whether any tab has a turn in flight.
int  chattabs_busy(void);

#endif
