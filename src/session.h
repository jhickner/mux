
#ifndef SESSION_H
#define SESSION_H

#include "tty.h"
#include "vendor/agents/backend.h"

struct session;

typedef int (*session_key_fn)(void *ud, tty_event *ev);
void session_set_typeahead(session_key_fn fn, void *ud);

int session_poll_input(void);

struct session *session_new(const char *backend, const char *cwd, const char *model,
                            const char *effort);
void            session_free(struct session *s);

void            session_replay(struct session *s);

void session_set_quiet(struct session *s, int quiet);

// Silent: like quiet, but nothing at all is written to the terminal. What the
// turn produced reaches the user some other way -- the observer below, and
// session_last_reply() at the end.
void session_set_silent(struct session *s, int silent);

// A second pair of eyes on the raw backend events, for a front end that is not
// the terminal. Called for every event, whether or not the terminal is drawing.
typedef void (*session_event_fn)(void *ud, const backend_event *ev);
void session_set_observer(struct session *s, session_event_fn fn, void *ud);

// Appended to the system prompt the backend is opened with. Copied; takes
// effect the next time the agent process starts.
void session_set_system_extra(struct session *s, const char *text);

// Polled alongside the keyboard while a turn is in flight: nonzero abandons it.
// This is how a front end other than the terminal says stop.
void session_set_abort_hook(struct session *s, int (*fn)(void *ud), void *ud);

void session_set_naming(struct session *s, int on);

void session_set_thinking(struct session *s, int on);
int  session_thinking(const struct session *s);

void session_set_compact(struct session *s, int on);
int  session_compact(const struct session *s);

void session_set_customizations(struct session *s, int on);

void session_set_fork(struct session *s, int on);

int session_start(struct session *s);

int session_trust_project(struct session *s);
int session_take_trust_request(struct session *s);

// Whose events may draw. The window sets this around anything that renders
// for one of its sessions, and puts back what it got. NULL means nothing
// draws. Set to a session other than the one an event belongs to, that event
// is kept but not drawn.
struct session *session_set_drawing(struct session *s);

int session_turn(struct session *s, const char *text);

// The same turn, run on its own thread so the window is free while it happens.
// Nothing is drawn from that thread: the events it produces are queued and
// drawn by session_turn_pump(), which the front end calls when the descriptor
// below wakes. A session with a turn in flight must be pumped this way and not
// through session_idle_pump(), which would take the turn's own stream.
int  session_turn_begin(struct session *s, const char *text);
int  session_turn_running(const struct session *s);
int  session_turn_pump(struct session *s);
int  session_wake_fd(const struct session *s);
double session_turn_elapsed(const struct session *s);

// Waits for a threaded turn to end, drawing what it produces as it waits. For
// a front end that has to run a turn of its own on this thread.
void session_turn_wait(struct session *s);

// Abandon the turn in flight, as escape does to one in the foreground.
void session_interrupt(struct session *s);

// A turn of its own, or work the agent still has running.
int  session_busy(const struct session *s);

int  session_idle_fd(const struct session *s);

int  session_idle_pump(struct session *s);
int  session_idle_busy(const struct session *s);

int session_switch_backend(struct session *s, const char *backend);

const char *session_failed_prompt(const struct session *s);

int session_clear(struct session *s);

int session_set_cwd(struct session *s, const char *path);

int session_set_model(struct session *s, const char *model);

int session_set_effort(struct session *s, const char *effort);
const char *session_effort(const struct session *s);
int session_can_set_effort(const struct session *s);

int session_set_permission(struct session *s, const char *mode);
const char *session_permission(const struct session *s);

int         session_permission_count(void);
const char *session_permission_name(int index);
const char *session_permission_desc(int index);
int         session_permission_index(const char *mode);
int         session_permission_default(void);

int session_resume(struct session *s, const char *id);

void session_adopt_id(struct session *s, const char *id);

const char *session_title(const struct session *s);

// Names this session. With a name, it is set outright; with none, the model is
// asked to name it again from the last turn. Nonzero if the rename took.
int session_rename(struct session *s, const char *name);

const char *session_model(const struct session *s);
const char *session_id(const struct session *s);

const char *session_saved_model(const char *backend);
const char *session_saved_effort(const char *backend);

const char *session_model_label(const struct session *s);

const char *session_model_short(const struct session *s, const char *model);

const char *session_effort_label(const struct session *s);

int session_can_resume(const struct session *s);
const char *session_cwd(const struct session *s);

const char *session_workdir(const struct session *s);
const char *session_backend(const struct session *s);

// The argv that reopens this session, program name excluded: -b, and whatever
// of -C/-m/-e/-s/--session applies. `what` picks the parts that belong to the
// caller's situation. Returns the count; the pointers are the session's own,
// so they live as long as it does. SESSION_ARGV_MAX entries are enough.
#define SESSION_ARGV_MAX 11
enum {
    SESSION_ARGV_CWD    = 1u << 0, // -C: for a process that will not start there
    SESSION_ARGV_RESUME = 1u << 1, // --session: pick the conversation up again
    SESSION_ARGV_SAFE   = 1u << 2, // -s: keep customizations off
};
int session_argv(const struct session *s, char **out, int max, unsigned what);
const char *session_last_reply(const struct session *s);
const char *session_last_error(const struct session *s);
int         session_last_interrupted(const struct session *s);

int session_context_percent(const struct session *s);

long session_context_window(const struct session *s);

// Sets the spinner's word from this session's effort. For the front end that
// runs the spinner itself, around a turn of its own.
void session_spin_word(const struct session *s);

void session_report(const struct session *s);

#endif
