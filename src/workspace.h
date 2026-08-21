#ifndef WORKSPACE_H
#define WORKSPACE_H

struct session;

#define WORKSPACE_MAX 12

// The sessions this window holds. There is one workspace per process: the tab
// on screen is the session the prompt talks to, and the others keep their own
// screen, their own agent, and their own turn.
int  workspace_begin(struct session *first, int safe_mode);
void workspace_end(void);

struct session *workspace_current(void);
struct session *workspace_at(int index);
int  workspace_count(void);
int  workspace_index(void);
int  workspace_index_of(const struct session *s);

// Starts a session with this window's defaults and opens it as a tab. `id`
// resumes a conversation when the backend can. Returns the new index or -1.
int  workspace_spawn(const char *backend, const char *model, const char *effort,
                     const char *cwd, const char *id);

// Adopts an already-started session as a tab.
int  workspace_open(struct session *s);

void workspace_show(int index);

// The tab holding a conversation, by the backend's id for it.
int  workspace_find_id(const char *id);

// Dumps one tab's screen, whether or not it is the one showing.
int  workspace_dump(int index, const char *path);

// Frees the session and drops the tab. Zero when it was the last one, which is
// the caller's cue to leave.
int  workspace_close(int index);


// Sends a line to a tab. A tab already running a turn takes it when that turn
// ends, so a follow-up can be typed without waiting. `shown` is what the sticky
// prompt says it is; NULL means the line itself.
int  workspace_send(int index, const char *line, const char *shown);
int  workspace_queued(int index);

// Waits for a tab's turn to end, for a caller that has to run one of its own
// on this thread. Its output goes to its own screen, as ever.
void workspace_settle(struct session *s);

// What runs when a tab's turn ends, whichever tab it was.
void workspace_on_finish(void (*fn)(struct session *s));

// Idle work for every tab, each rendering into its own screen.
int  workspace_fds(int *out, int max);
int  workspace_pump(void);
int  workspace_busy(void);

// working | errored | finished, as the registry spells it.
const char *workspace_status(const struct session *s);

// The tab strip, painted as chrome once the window holds more than one.
int  workspace_strip_rows(void);
void workspace_strip_paint(void);

#endif
