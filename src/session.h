
#ifndef SESSION_H
#define SESSION_H

#include "tty.h"

struct session;

typedef int (*session_key_fn)(void *ud, tty_event *ev);
void session_set_typeahead(session_key_fn fn, void *ud);

int session_poll_input(void);

struct session *session_new(const char *backend, const char *cwd, const char *model,
                            const char *effort);
void            session_free(struct session *s);

void            session_replay(struct session *s);

void session_set_quiet(struct session *s, int quiet);

void session_set_naming(struct session *s, int on);

void        session_hold_footer(struct session *s, int on);

const char *session_footer(const struct session *s);

void session_set_thinking(struct session *s, int on);
int  session_thinking(const struct session *s);

void session_set_compact(struct session *s, int on);
int  session_compact(const struct session *s);

void session_set_customizations(struct session *s, int on);

int session_start(struct session *s);

int session_turn(struct session *s, const char *text);

int  session_idle_fd(const struct session *s);

int  session_idle_pump(struct session *s);
int  session_idle_busy(const struct session *s);

int session_switch_backend(struct session *s, const char *backend);

const char *session_failed_prompt(const struct session *s);

int session_clear(struct session *s);

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

int session_context_percent(const struct session *s);

long session_context_window(const struct session *s);

void session_report(const struct session *s);

#endif
