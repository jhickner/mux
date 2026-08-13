/* Turn status for the tmux-agent-tabs plugin, so this pane's tab shows a
 * spinner while a turn runs. The CLI we drive fires the plugin's own lifecycle
 * hook, but not when we interrupt a turn — the SDK interrupt ends the turn with
 * no Stop event, leaving that hook's record stuck on "working" and the tab
 * spinning. We know every turn boundary exactly, for every backend, so we take
 * the tab over and report it ourselves. */
#ifndef AGENTTABS_H
#define AGENTTABS_H

/* Claim this pane's tab and record a session waiting for its first prompt.
 * Must run before the CLI is spawned: the child inherits the marker that
 * silences its hook. Outside tmux, or with the plugin not installed, this and
 * everything below is a no-op. */
void agenttabs_begin(void);

/* The three states the plugin renders for us: a spinner, nothing (a finished
 * turn, or a dot if you were on another tab), and an error marker. */
void agenttabs_working(void);
void agenttabs_finished(void);
void agenttabs_errored(void);

/* Discard the hook record left by an earlier run of session `id` — one from
 * before we started silencing the hook. It is keyed by session id, so resuming
 * that conversation would join a stale status to our live process, and the
 * plugin keeps the most attention-worthy status per window. */
void agenttabs_forget_hook(const char *id);

#endif /* AGENTTABS_H */
