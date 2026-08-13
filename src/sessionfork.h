/* Forking the conversation: a second agent, resumed on the same context, in its
 * own git worktree and its own tmux pane. */
#ifndef SESSIONFORK_H
#define SESSIONFORK_H

struct session;

/* Remember how this program was invoked, so a fork can re-exec it. Call once
 * from main() with argv[0]. */
void sessionfork_set_program(const char *argv0);

/* Where the forked agent lands. The split names follow tmux's own flags: a
 * horizontal split puts the panes side by side. */
enum fork_where {
    FORK_SPLIT_H,
    FORK_SPLIT_V,
    FORK_WINDOW,
};

/* Branch a worktree off HEAD, carry over uncommitted tracked changes, and open
 * a tmux pane there running this program resumed on the current conversation.
 * Returns 1 on success; reports its own errors. */
int sessionfork(const struct session *s, enum fork_where where);

/* When this agent is itself running in a linked worktree, print the command
 * that reopens this conversation there. Call on the way out, while the display
 * is still up. Does nothing anywhere else. */
void sessionfork_exit_note(const struct session *s);

#endif /* SESSIONFORK_H */
