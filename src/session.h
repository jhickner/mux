/* The conversation: one persistent agent CLI process behind backend.h, the
 * running totals for its turns, and the live rendering of a turn as it
 * streams. */
#ifndef SESSION_H
#define SESSION_H

#include "tty.h"

struct session;

/* Handler for keys typed while a turn is in flight — it owns `ev` (including
 * ev.text) and returns nonzero to interrupt the turn. Without one, Escape,
 * Ctrl-C and EOF interrupt and everything else is discarded. */
typedef int (*session_key_fn)(void *ud, tty_event *ev);
void session_set_typeahead(session_key_fn fn, void *ud);

/* `backend` names the agent CLI to drive — "claude", "codex", "grok" or "pi",
 * NULL for claude. `cwd` is where it runs its tools; `model` and `effort` may
 * be NULL for the CLI defaults. No pointer is retained. */
struct session *session_new(const char *backend, const char *cwd, const char *model,
                            const char *effort);
void            session_free(struct session *s);

/* Print only the reply text: no spinner, tool activity, or footer. For piped
 * output and one-shot runs. */
void session_set_quiet(struct session *s, int quiet);

/* Show the ✻ rows carrying the model's reasoning as it streams. On by default;
 * only the backends that report reasoning at all are affected. */
void session_set_thinking(struct session *s, int on);
int  session_thinking(const struct session *s);

/* Give every tool call the one-row collapsed layout — no output preview and no
 * diff — instead of only the calls that just read. Off by default. */
void session_set_compact(struct session *s, int on);
int  session_compact(const struct session *s);

/* Load the user's discovered customizations — skills, CLAUDE.md, plugins,
 * hooks, MCP servers, custom commands and agents. Must be set before
 * session_start(). */
void session_set_customizations(struct session *s, int on);

/* Spawn (or respawn) the CLI process. Returns 0 on failure — an unknown
 * backend name, or a CLI that is not installed. */
int session_start(struct session *s);

/* Run one turn, printing activity, the reply, and the stats footer. Returns 0
 * when the turn failed; an interactive caller may still switch backends. */
int session_turn(struct session *s, const char *text);

/* The agent can take a turn with nobody having asked — a background task it
 * started finishes and wakes it. session_idle_fd() is readable when such a turn
 * has output waiting, or -1 when this backend has no such turns or is not up
 * yet; session_idle_pump() prints what has arrived, the same way a turn's own
 * activity is printed. Only for use between turns, with the caller's own
 * display out of the way. */
int  session_idle_fd(const struct session *s);
void session_idle_pump(struct session *s);

/* Start a fresh provider session carrying the completed turns observed by this
 * process as developer/system context. The old backend remains live if the new
 * one cannot start. The target uses its default model. */
int session_switch_backend(struct session *s, const char *backend);

/* The prompt from the last failed provider turn, if any. It remains owned by
 * the session and is useful for retrying immediately after a backend switch. */
const char *session_failed_prompt(const struct session *s);

/* Drop the conversation context, keeping the process alive. */
int session_clear(struct session *s);

/* Switch models by restarting the CLI on the current session id, so context
 * carries over. Returns 0 on failure, leaving the old client in place. */
int session_set_model(struct session *s, const char *model);

/* Switch reasoning/thinking effort. Live-capable backends preserve their
 * process; the others restart and resume like session_set_model(). */
int session_set_effort(struct session *s, const char *effort);
const char *session_effort(const struct session *s);
int session_can_set_effort(const struct session *s);

/* How the CLI gates tool calls, claude only. Headless has no way to answer an
 * approval prompt, so a mode that would ask instead refuses the call and tells
 * the model — "bypassPermissions" is the one that never refuses. Restarts the
 * CLI on the current session id like session_set_model. Returns 0 on failure,
 * leaving the old client in place. */
int session_set_permission(struct session *s, const char *mode);
const char *session_permission(const struct session *s);

/* The modes the claude CLI accepts, indexed for the settings file; NULL past
 * the end. Index 0 is the default. */
const char *session_permission_name(int index);
int         session_permission_index(const char *mode);

/* Restart on a past session id, adopting its context. Only the backends that
 * can resume a conversation (claude, codex, grok, pi) carry anything across. */
int session_resume(struct session *s, const char *id);

/* Adopt a session id before session_start(), so the CLI comes up already
 * resumed instead of being spawned twice. */
void session_adopt_id(struct session *s, const char *id);

const char *session_model(const struct session *s);
const char *session_id(const struct session *s);

/* Whether this backend can pick a conversation back up from its session id, as
 * session_resume() and forking both need. */
int session_can_resume(const struct session *s);
const char *session_cwd(const struct session *s);
const char *session_backend(const struct session *s);
const char *session_last_reply(const struct session *s);

/* How much of the context window is occupied, 0-100, or -1 before the backend
 * has reported enough to say. Tracks the running turn where the driver reports
 * usage as it goes, so it is safe to call from a repaint. */
int session_context_percent(const struct session *s);

/* Print the /session report: model, id, cwd, turns, tokens, cost. */
void session_report(const struct session *s);

#endif /* SESSION_H */
