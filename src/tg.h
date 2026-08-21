#ifndef TG_H
#define TG_H

struct session;

// The Telegram bridge: a second front end on the same session. Messages from
// the chat are dispatched exactly as if they had been typed at the prompt, and
// what the agent does is mirrored back as it happens.
//
// The bot token comes from $TELEGRAM_TOKEN; everything else lives in
// ~/.config/mux/telegram (chat_id, mirror, artifacts_*).

// Attach to `s`. `headless` when there is no terminal to share with, which is
// the daemon case: the chat is then the only front end. Nonzero on success.
int tg_start(struct session *s, int headless);
void tg_stop(void);

// "telegram @bot" while the bridge is live, NULL when it is not. The hud and
// /session show it: which front ends are attached is not otherwise visible.
const char *tg_label(void);

// The poller's wake pipe, for the prompt's idle select().
int tg_fds(int *out, int max);

// A line the chat has sent, to run as if typed. Main thread; caller frees.
char *tg_take_line(void);

// Whether a line is waiting — the prompt uses this to break out of its poll.
int tg_pending(void);

// Run a line the chat sent, on the thread that owns the session. Takes `line`.
void tg_run_line(char *line);

// True while a chat line is being dispatched: the front end behind this line
// has no keyboard, so nothing may wait on one.
int tg_line_in_flight(void);

// The session the bridge is attached to, or NULL.
struct session *tg_session(void);

// `s` is about to be freed or handed to another window: drop the cached
// pointer if it is the one the bridge is holding.
void tg_forget_session(struct session *s);

// Headless: run the chat's own loop until it is told to stop.
void tg_run(struct session *s);

// What the agent must be told about this front end: the chat, the artifact
// links, the reminder store, and that long work belongs in a subagent.
const char *tg_system_note(void);

#endif
