#ifndef LIVELIST_H
#define LIVELIST_H

struct session;

// One conversation a running mux is holding. Every interactive mux publishes a
// record per session, so any window can see what the others have open and ask
// for one of them.
struct live_session {
    long pid;
    int  slot;
    char backend[32];
    char model[160];    /* as it was asked for: may be "default" */
    char label[160];    /* as the CLI resolved it, for showing */
    char effort[32];
    char cwd[4096];
    char id[128];
    char title[200];
    char status[16];    /* working | finished | errored */
    char pane[32];
    char window[32];    /* tmux window id, "@3": which window holds it */
    char wname[64];     /* that window as tmux shows it, "3:mux" */
    char pane_index[8];
    long ts;
    int  mine;          /* held by this process */
};

// Starts publishing; the records this process wrote go when it exits.
void livelist_begin(void);

void livelist_publish(const struct session *s, const char *status);

// The session was closed, or handed to another window.
void livelist_forget(const struct session *s);

// Every live session on this machine, newest first, with dead pids pruned.
// The caller frees the array.
int livelist_load(struct live_session **out);

int livelist_alive(long pid);

// This window as tmux names it, for telling our own panes from other windows'.
// Both are empty outside tmux.
const char *livelist_tmux_window(void);
const char *livelist_tmux_window_name(void);

#endif
