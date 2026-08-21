
#ifndef SESSIONFORK_H
#define SESSIONFORK_H

struct session;

void sessionfork_set_program(const char *argv0);

const char *sessionfork_program(void);

enum fork_where {
    FORK_SPLIT_H,
    FORK_SPLIT_V,
    FORK_WINDOW,
};

int sessionfork_run(const struct session *s, enum fork_where where);

// A plain shell beside this one, in the directory the session works in.
// Nothing of the conversation goes with it.
int sessionfork_shell(const struct session *s, enum fork_where where, int quiet);

void sessionfork_exit_note(const struct session *s);

#endif
