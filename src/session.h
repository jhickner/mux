
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

int session_turn(struct session *s, const char *text);

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
const char *session_last_reply(const struct session *s);
const char *session_last_error(const struct session *s);
int         session_last_interrupted(const struct session *s);

int session_context_percent(const struct session *s);

long session_context_window(const struct session *s);

void session_report(const struct session *s);

#endif
