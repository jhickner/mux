/*
 * pi.h — drive the pi coding agent (`pi --mode rpc`) from C.
 *
 * A persistent subprocess speaks pi's JSONL RPC protocol over stdin/stdout.
 * pi_send() blocks until the agent is fully settled and returns the accumulated
 * assistant text for that turn. Context, tools, skills, and compaction remain
 * in the one live pi session.
 *
 *     #define PI_IMPLEMENTATION       // in exactly one .c file
 *     #include "pi.h"
 *
 *     pi_opts o = {0};
 *     o.cwd = "/path/to/project";
 *     o.model = "openrouter/pareto-code";
 *     pi_client *c = pi_start(&o);
 *     char *reply = pi_send(c, "inspect this project and fix the tests");
 *     puts(reply); free(reply);
 *     pi_stop(c);
 *
 * Depends on cJSON (bundled beside this header) and the `pi` CLI on PATH.
 *
 * Optional Backend adapter: include backend.h first, then define both
 * PI_IMPLEMENTATION and PI_BACKEND_IMPLEMENTATION before this header. This
 * exposes pi_backend_open(opts), which returns a Backend filled in the same way
 * as the claude, codex, and grok adapters.
 */
#ifndef PI_H
#define PI_H

typedef struct pi_client pi_client;

typedef struct {
    const char *cli_path;      /* pi binary; NULL/"" -> "pi" via PATH          */
    const char *cwd;           /* child working directory; NULL -> inherit     */
    const char *provider;      /* --provider value; NULL -> pi default         */
    const char *model;         /* --model value; NULL -> pi default            */
    const char *effort;        /* --thinking value; NULL -> pi default         */
    const char *append_system; /* prepended to each prompt; NULL -> none       */
    const char *resume_session;/* --session id; NULL -> a fresh conversation   */
    int no_session;            /* nonzero -> pass --no-session. Also the
                                  default when opts is NULL.                   */
    int no_tools;              /* nonzero -> pass --no-tools                   */
} pi_opts;

/* Start a persistent `pi --mode rpc` process. Returns NULL only on a local
 * setup failure; a missing/broken pi executable is reported by the first send.
 */
pi_client *pi_start(const pi_opts *opts);

/* Send one plain-text prompt and wait for the complete agent operation
 * (`agent_settled`). Returns accumulated assistant text (malloc'd; caller
 * frees), or NULL if pi rejects the prompt or exits. An interrupted operation
 * returns the text produced so far (possibly empty). Context is retained
 * across calls. */
char *pi_send(pi_client *c, const char *user_text);

typedef struct {
    int interrupted;   /* the abort predicate ended the operation */
} pi_result;

/* As pi_send, but also fills *meta (zeroed first). `meta` may be NULL. */
char *pi_send_ex(pi_client *c, const char *user_text, pi_result *meta);

/* Start a fresh in-memory pi session, discarding conversation context. Returns
 * nonzero on success. This does not restart the subprocess. */
int pi_reset(pi_client *c);

/* The session id pi reports via get_state, or NULL until known. Needed to
 * resume later with --session. Absent when the process was started ephemeral. */
const char *pi_session_id(pi_client *c);

/* When on, pi_send writes a compact event trace to stderr. */
void pi_set_verbose(pi_client *c, int on);

/* One interesting event from the stream. All strings are borrowed for the
 * callback. Tool arguments are the compact JSON object sent by pi. */
typedef enum {
    PI_EV_ASSISTANT,
    PI_EV_THINKING,
    PI_EV_TOOL,
    PI_EV_TOOL_RESULT,
} pi_event_kind;

typedef struct {
    pi_event_kind kind;
    const char *text;       /* assistant/thinking text, or tool result */
    const char *name;       /* tool name, for PI_EV_TOOL              */
    const char *input_json; /* tool arguments, for PI_EV_TOOL         */
} pi_event;

/* Per-event callback while pi_send is running. */
void pi_set_event_cb(pi_client *c,
                     void (*cb)(void *ud, const pi_event *ev),
                     void *ud);

/* Predicate polled while awaiting pi output. A nonzero result aborts the
 * current operation (pi receives an abort command before pi_send returns). */
void pi_set_abort_check(pi_client *c, int (*cb)(void));

/* Send an asynchronous abort command. Normally callers use abort_check. */
int pi_abort(pi_client *c);

/* Change the live session's thinking level without losing context. NULL
 * restores the level that was active before the first change. */
int pi_set_effort(pi_client *c, const char *effort);

/* Close stdin, terminate/reap the child if needed, and free the client. */
void pi_stop(pi_client *c);

/* Available only when backend.h was included before pi.h. */
#ifdef BACKEND_H
Backend *pi_backend_open(const backend_opts *opts);
#endif

#endif /* PI_H */

/* ======================================================================== */
/*   IMPLEMENTATION                                                          */
/* ======================================================================== */
#ifdef PI_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/wait.h>
#include "cJSON.h"

/* How long a read waits on the child before running the abort predicate again.
 * Also the worst-case delay before a key the caller reads from that predicate
 * reaches the screen, so it sits below the ~30ms an echo can take without being
 * felt as lag. */
#define PI_TICK_MS 20

struct pi_client {
    pid_t pid;
    int in_fd, out_fd;
    int next_id, verbose;
    int (*abort)(void);
    void (*on_event)(void *ud, const pi_event *ev);
    void *on_event_ud;
    char *sys, *default_effort;
    char *buf;
    size_t len, cap;
    char session_id[128];
};

void pi_set_verbose(pi_client *c, int on) { if (c) c->verbose = on; }
void pi_set_abort_check(pi_client *c, int (*cb)(void)) { if (c) c->abort = cb; }
void pi_set_event_cb(pi_client *c,
                     void (*cb)(void *ud, const pi_event *ev),
                     void *ud) {
    if (c) { c->on_event = cb; c->on_event_ud = ud; }
}

static void pi_emit(pi_client *c, const pi_event *ev) {
    static const char *const names[] = {
        "assistant", "thinking", "tool", "tool-result"
    };
    const char *preview = ev->kind == PI_EV_TOOL ? ev->name : ev->text;
    if (!preview) preview = "";
    if (c->verbose) {
        const char *kind = names[ev->kind];
        fprintf(stderr, "  [%s] %.400s%s\n", kind, preview,
                strlen(preview) > 400 ? " ..." : "");
    }
    if (c->on_event) c->on_event(c->on_event_ud, ev);
}

static int pi_write(pi_client *c, cJSON *obj) {
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json) return 0;
    size_t n = strlen(json);
    char *line = realloc(json, n + 2);
    if (!line) { free(json); return 0; }
    line[n++] = '\n';
    int ok = 1;
    for (size_t off = 0; off < n;) {
        ssize_t w = write(c->in_fd, line + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; ok = 0; break; }
        off += (size_t)w;
    }
    free(line);
    return ok;
}

/* Caller owns params; pi_write owns and frees the containing object. */
static int pi_command(pi_client *c, const char *type, cJSON *params) {
    cJSON *o = params ? params : cJSON_CreateObject();
    int id = c->next_id++;
    char idbuf[32];
    snprintf(idbuf, sizeof idbuf, "req-%d", id);
    cJSON_AddStringToObject(o, "id", idbuf);
    cJSON_AddStringToObject(o, "type", type);
    return pi_write(c, o) ? id : 0;
}

/* Read one complete JSONL object. A return of -1 is EOF/error, 0 is an abort
 * requested by the caller, and 1 supplies a cJSON object owned by *out. */
static int pi_read(pi_client *c, cJSON **out, int honor_abort) {
    char tmp[8192];
    *out = NULL;
    for (;;) {
        char *nl = memchr(c->buf, '\n', c->len);
        if (nl) {
            size_t n = (size_t)(nl - c->buf);
            if (n && c->buf[n - 1] == '\r') n--;
            char saved = c->buf[n];
            c->buf[n] = '\0';
            *out = n ? cJSON_Parse(c->buf) : NULL;
            c->buf[n] = saved;
            size_t used = (size_t)(nl + 1 - c->buf);
            memmove(c->buf, c->buf + used, c->len - used);
            c->len -= used;
            if (*out) return 1;              /* ignore blank/malformed records */
            continue;
        }
        if (honor_abort && c->abort && c->abort()) return 0;
        struct pollfd pfd = { c->out_fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, PI_TICK_MS);
        if (pr < 0) { if (errno == EINTR) continue; return -1; }
        if (!pr) continue;
        ssize_t r = read(c->out_fd, tmp, sizeof tmp);
        if (r <= 0) return -1;
        if (c->len + (size_t)r + 1 > c->cap) {
            size_t nc = (c->len + (size_t)r + 1) * 2;
            char *nb = realloc(c->buf, nc);
            if (!nb) return -1;
            c->buf = nb; c->cap = nc;
        }
        memcpy(c->buf + c->len, tmp, (size_t)r);
        c->len += (size_t)r;
    }
}

static void pi_append(char **dst, const char *s) {
    if (!s) return;
    size_t a = *dst ? strlen(*dst) : 0, b = strlen(s);
    char *n = realloc(*dst, a + b + 1);
    if (!n) return;
    memcpy(n + a, s, b + 1);
    *dst = n;
}

static const char *pi_text(cJSON *result) {
    cJSON *content = result ? cJSON_GetObjectItemCaseSensitive(result, "content") : NULL;
    if (!cJSON_IsArray(content)) return NULL;
    cJSON *block;
    cJSON_ArrayForEach(block, content) {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(block, "type");
        if (cJSON_IsString(type) && !strcmp(type->valuestring, "text")) {
            cJSON *text = cJSON_GetObjectItemCaseSensitive(block, "text");
            if (cJSON_IsString(text)) return text->valuestring;
        }
    }
    return NULL;
}

/* Consume display-worthy RPC events. settled is set for agent_settled. */
static void pi_consume_event(pi_client *c, cJSON *ev, char **acc, int *settled) {
    cJSON *type = cJSON_GetObjectItemCaseSensitive(ev, "type");
    if (!cJSON_IsString(type)) return;
    if (!strcmp(type->valuestring, "agent_settled")) { *settled = 1; return; }
    if (!strcmp(type->valuestring, "message_update")) {
        cJSON *delta = cJSON_GetObjectItemCaseSensitive(ev, "assistantMessageEvent");
        cJSON *dt = delta ? cJSON_GetObjectItemCaseSensitive(delta, "type") : NULL;
        const char *d = delta ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(delta, "delta")) : NULL;
        if (cJSON_IsString(dt) && d) {
            if (!strcmp(dt->valuestring, "text_delta")) {
                pi_event out = { .kind = PI_EV_ASSISTANT, .text = d };
                pi_append(acc, d); pi_emit(c, &out);
            } else if (!strcmp(dt->valuestring, "thinking_delta")) {
                pi_event out = { .kind = PI_EV_THINKING, .text = d };
                pi_emit(c, &out);
            }
        }
    } else if (!strcmp(type->valuestring, "tool_execution_start")) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(ev, "toolName"));
        cJSON *args = cJSON_GetObjectItemCaseSensitive(ev, "args");
        char *input = args ? cJSON_PrintUnformatted(args) : NULL;
        pi_event out = {
            .kind = PI_EV_TOOL,
            .name = name ? name : "tool",
            .input_json = input,
        };
        pi_emit(c, &out);
        free(input);
    } else if (!strcmp(type->valuestring, "tool_execution_end")) {
        const char *text = pi_text(cJSON_GetObjectItemCaseSensitive(ev, "result"));
        pi_event out = { .kind = PI_EV_TOOL_RESULT,
                         .text = text ? text : "(result)" };
        pi_emit(c, &out);
    }
}

static int pi_response(cJSON *ev, int id) {
    cJSON *type = cJSON_GetObjectItemCaseSensitive(ev, "type");
    cJSON *jid = cJSON_GetObjectItemCaseSensitive(ev, "id");
    char want[32];
    snprintf(want, sizeof want, "req-%d", id);
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "response") ||
        !cJSON_IsString(jid) || strcmp(jid->valuestring, want)) return -1;
    cJSON *success = cJSON_GetObjectItemCaseSensitive(ev, "success");
    return cJSON_IsTrue(success) ? 1 : 0;
}

static int pi_wait_response(pi_client *c, int id) {
    for (;;) {
        cJSON *ev;
        int r = pi_read(c, &ev, 0);
        if (r != 1) return 0;
        int response = pi_response(ev, id);
        cJSON_Delete(ev);
        if (response >= 0) return response;
    }
}

/* As pi_wait_response, copying one string from the response's data object. */
static int pi_wait_response_string(pi_client *c, int id, const char *key,
                                   char **value) {
    for (;;) {
        cJSON *ev;
        int r = pi_read(c, &ev, 0);
        if (r != 1) return 0;
        int response = pi_response(ev, id);
        if (response >= 0) {
            const cJSON *data = cJSON_GetObjectItemCaseSensitive(ev, "data");
            const char *s = data ? cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(data, key)) : NULL;
            if (response && s) *value = strdup(s);
            cJSON_Delete(ev);
            return response && s != NULL && *value != NULL;
        }
        cJSON_Delete(ev);
    }
}

/* Pull sessionId out of a get_state response. The object is the full event. */
static void pi_take_id(pi_client *c, cJSON *ev) {
    cJSON *data = cJSON_GetObjectItemCaseSensitive(ev, "data");
    const char *id = data ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(data, "sessionId")) : NULL;
    if (id && *id)
        snprintf(c->session_id, sizeof c->session_id, "%s", id);
}

/* Ask for the current session id. Safe between turns; not while a prompt is
 * in flight, because pi_read would steal that turn's events. */
static void pi_refresh_id(pi_client *c) {
    int id = pi_command(c, "get_state", NULL);
    if (!id) return;
    for (;;) {
        cJSON *ev;
        int r = pi_read(c, &ev, 0);
        if (r != 1) return;
        int response = pi_response(ev, id);
        if (response == 1) pi_take_id(c, ev);
        cJSON_Delete(ev);
        if (response >= 0) return;
    }
}

pi_client *pi_start(const pi_opts *opts) {
    pi_opts o = opts ? *opts : (pi_opts){0};
    const char *cli = (o.cli_path && *o.cli_path) ? o.cli_path : "pi";
    signal(SIGPIPE, SIG_IGN);
    int in[2] = { -1, -1 }, out[2] = { -1, -1 };
    if (pipe(in) != 0) return NULL;
    if (pipe(out) != 0) { close(in[0]); close(in[1]); return NULL; }
    pid_t pid = fork();
    if (pid < 0) { close(in[0]); close(in[1]); close(out[0]); close(out[1]); return NULL; }
    if (pid == 0) {
        if (dup2(in[0], STDIN_FILENO) < 0 || dup2(out[1], STDOUT_FILENO) < 0) _exit(126);
        if (in[0] != STDIN_FILENO) close(in[0]); if (in[1] != STDIN_FILENO) close(in[1]);
        if (out[0] != STDOUT_FILENO) close(out[0]); if (out[1] != STDOUT_FILENO) close(out[1]);
        if (o.cwd && *o.cwd && chdir(o.cwd)) _exit(126);
        const char *argv[22]; int n = 0;
        argv[n++] = cli; argv[n++] = "--mode"; argv[n++] = "rpc";
        if (o.resume_session && *o.resume_session) {
            argv[n++] = "--session"; argv[n++] = o.resume_session;
        } else if (o.no_session || !opts) {
            argv[n++] = "--no-session";
        }
        if (o.no_tools) argv[n++] = "--no-tools";
        argv[n++] = "--no-extensions";
        argv[n++] = "--no-skills";
        argv[n++] = "--no-prompt-templates";
        argv[n++] = "--no-themes";
        argv[n++] = "--no-context-files";
        if (o.provider && *o.provider) { argv[n++] = "--provider"; argv[n++] = o.provider; }
        if (o.model && *o.model) { argv[n++] = "--model"; argv[n++] = o.model; }
        if (o.effort && *o.effort) { argv[n++] = "--thinking"; argv[n++] = o.effort; }
        argv[n] = NULL;
        execvp(cli, (char *const *)argv); _exit(127);
    }
    close(in[0]); close(out[1]);
    pi_client *c = calloc(1, sizeof *c);
    if (!c) { close(in[1]); close(out[0]); kill(pid, SIGKILL); waitpid(pid, NULL, 0); return NULL; }
    c->pid = pid; c->in_fd = in[1]; c->out_fd = out[0]; c->next_id = 1;
    c->sys = (o.append_system && *o.append_system) ? strdup(o.append_system) : NULL;
    if (o.resume_session && *o.resume_session)
        snprintf(c->session_id, sizeof c->session_id, "%s", o.resume_session);
    fcntl(c->in_fd, F_SETFD, FD_CLOEXEC); fcntl(c->out_fd, F_SETFD, FD_CLOEXEC);
    /* Don't block on get_state: pi is still booting, and the UI comes up as
     * soon as this returns. The id is known already on --session; a fresh one
     * is asked for after the first turn (and after reset). A child that died
     * during exec is the only failure we can see this early. */
    if (waitpid(c->pid, NULL, WNOHANG) == c->pid) {
        close(c->in_fd); close(c->out_fd);
        free(c->sys); free(c->buf); free(c);
        return NULL;
    }
    return c;
}

const char *pi_session_id(pi_client *c) {
    return (c && c->session_id[0]) ? c->session_id : NULL;
}

int pi_abort(pi_client *c) { return c ? pi_command(c, "abort", NULL) != 0 : 0; }

int pi_set_effort(pi_client *c, const char *effort) {
    if (!c) return 0;
    if (!c->default_effort) {
        int state_id = pi_command(c, "get_state", NULL);
        if (!state_id || !pi_wait_response_string(c, state_id, "thinkingLevel",
                                                   &c->default_effort))
            return 0;
    }
    const char *level = effort && *effort ? effort : c->default_effort;
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "level", level);
    int id = pi_command(c, "set_thinking_level", params);
    return id && pi_wait_response(c, id);
}

char *pi_send_ex(pi_client *c, const char *user_text, pi_result *meta) {
    if (meta) memset(meta, 0, sizeof *meta);
    if (!c || !user_text) return NULL;
    char *full = NULL;
    if (c->sys) {
        size_t n = strlen(c->sys) + strlen(user_text) + 8;
        full = malloc(n);
        if (!full) return NULL;
        snprintf(full, n, "%s\n\n---\n\n%s", c->sys, user_text);
    }
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "message", full ? full : user_text);
    int id = pi_command(c, "prompt", params);
    free(full);
    if (!id) return NULL;

    int accepted = 0, settled = 0, aborted = 0;
    char *answer = NULL;
    while (!settled) {
        cJSON *ev;
        int r = pi_read(c, &ev, !aborted);
        if (r == 0 && !aborted) {
            if (!pi_abort(c)) { free(answer); return NULL; }
            if (meta) meta->interrupted = 1;
            aborted = 1;
            continue;
        }
        if (r != 1) { free(answer); return NULL; }
        int response = pi_response(ev, id);
        if (response == 0) { cJSON_Delete(ev); free(answer); return NULL; }
        if (response == 1) accepted = 1;
        pi_consume_event(c, ev, &answer, &settled);
        cJSON_Delete(ev);
    }
    if (!accepted) { free(answer); return NULL; }
    if (!c->session_id[0]) pi_refresh_id(c);
    return answer ? answer : strdup("");
}

char *pi_send(pi_client *c, const char *user_text) {
    return pi_send_ex(c, user_text, NULL);
}

int pi_reset(pi_client *c) {
    if (!c) return 0;
    int id = pi_command(c, "new_session", NULL);
    if (!id || !pi_wait_response(c, id)) return 0;
    c->session_id[0] = '\0';
    pi_refresh_id(c);
    return 1;
}

/* Poll for the child's exit for up to `ms`. Returns nonzero once reaped. */
static int pi_reap_within(pi_client *c, int ms) {
    for (int waited = 0; waited < ms; waited += 5) {
        if (waitpid(c->pid, NULL, WNOHANG) == c->pid) { c->pid = 0; return 1; }
        usleep(5000);
    }
    return 0;
}

void pi_stop(pi_client *c) {
    if (!c) return;
    if (c->in_fd >= 0) { close(c->in_fd); c->in_fd = -1; }  /* EOF asks it to exit */
    if (c->pid > 0) {
        /* We only stop between turns, so pi has nothing left to finish. Waiting
         * out its unwind costs about a second, which an interactive caller
         * feels on quit, so escalate immediately. */
        kill(c->pid, SIGTERM);
        if (!pi_reap_within(c, 50)) {
            kill(c->pid, SIGKILL);
            waitpid(c->pid, NULL, 0);
            c->pid = 0;
        }
    }
    if (c->out_fd >= 0) close(c->out_fd);
    free(c->sys); free(c->default_effort); free(c->buf); free(c);
}

#endif /* PI_IMPLEMENTATION */

/* ======================================================================== */
/*   OPTIONAL backend.h ADAPTER                                              */
/* ======================================================================== */
#ifdef PI_BACKEND_IMPLEMENTATION
#ifndef BACKEND_H
#error "Include backend.h before defining PI_BACKEND_IMPLEMENTATION."
#endif
#ifndef PI_IMPLEMENTATION
#error "PI_BACKEND_IMPLEMENTATION requires PI_IMPLEMENTATION in this translation unit."
#endif

typedef struct { backend_state st; pi_client *c; } pi_backend_ctx;

/* Pi streams text and reasoning as deltas. Tool starts already contain the
 * complete argument object, which the shared renderer uses to find paths. */
static void pi_backend_event(void *ud, const pi_event *pev) {
    pi_backend_ctx *cx = ((Backend *)ud)->ctx;
    if (!cx->st.on_event) return;
    if (pev->kind == PI_EV_ASSISTANT) {
        backend_delta(&cx->st, BACKEND_EV_ASSISTANT, pev->text);
    } else if (pev->kind == PI_EV_THINKING) {
        backend_delta(&cx->st, BACKEND_EV_THINKING, pev->text);
    } else {
        backend_flush(&cx->st);
        backend_event ev = {0};
        if (pev->kind == PI_EV_TOOL) {
            ev.kind = BACKEND_EV_TOOL;
            ev.name = pev->name;
            ev.input_json = pev->input_json;
        } else if (pev->kind == PI_EV_TOOL_RESULT) {
            ev.kind = BACKEND_EV_TOOL_RESULT;
            ev.text = pev->text;
        } else {
            return;
        }
        backend_emit(&cx->st, &ev);
    }
}

static int pi_backend_start(Backend *b, const char *resume) {
    pi_backend_ctx *cx = b->ctx;
    pi_opts o = {0};
    o.cwd = cx->st.cwd;
    o.model = cx->st.model;
    o.effort = cx->st.effort;
    o.append_system = cx->st.system;
    o.resume_session = resume;
    o.no_session = cx->st.ephemeral;
    o.no_tools = cx->st.disable_tools;
    pi_client *c = pi_start(&o);
    if (!c) return 0;
    pi_set_event_cb(c, pi_backend_event, b);
    pi_set_abort_check(c, cx->st.abort);
    if (cx->c) pi_stop(cx->c);
    cx->c = c;
    backend_set(&cx->st.resume, resume);
    return 1;
}

static char *pi_backend_ask_ex(Backend *b, const char *user, backend_result *meta) {
    pi_backend_ctx *cx = b->ctx;
    if (meta) memset(meta, 0, sizeof *meta);
    if (!cx->c && !pi_backend_start(b, cx->st.resume)) return NULL;
    pi_result pr = {0};
    char *reply = pi_send_ex(cx->c, user, &pr);
    backend_flush(&cx->st);
    if (meta) meta->interrupted = pr.interrupted;
    return reply;
}
static char *pi_backend_ask(Backend *b, const char *user) {
    return pi_backend_ask_ex(b, user, NULL);
}
static int pi_backend_reset(Backend *b) {
    pi_backend_ctx *cx = b->ctx;
    if (!cx->c) return pi_backend_start(b, NULL);
    if (pi_reset(cx->c)) return 1;
    pi_stop(cx->c); cx->c = NULL;
    return 0;
}
static void pi_backend_set_event_cb(Backend *b,
                                    void (*cb)(void *ud, const backend_event *ev),
                                    void *ud) {
    pi_backend_ctx *cx = b->ctx;
    cx->st.on_event = cb; cx->st.event_ud = ud;
    if (cx->c) pi_set_event_cb(cx->c, pi_backend_event, b);
}
static void pi_backend_set_abort(Backend *b, int (*cb)(void)) {
    pi_backend_ctx *cx = b->ctx;
    cx->st.abort = cb;
    if (cx->c) pi_set_abort_check(cx->c, cb);
}
static int pi_backend_set_effort(Backend *b, const char *effort) {
    pi_backend_ctx *cx = b->ctx;
    if (cx->c && !pi_set_effort(cx->c, effort)) return 0;
    backend_set(&cx->st.effort, effort);
    return 1;
}
static const char *pi_backend_session_id(Backend *b) {
    pi_backend_ctx *cx = b->ctx;
    return cx->c ? pi_session_id(cx->c) : NULL;
}
static void pi_backend_close(Backend *b) {
    pi_backend_ctx *cx = b->ctx;
    if (cx->c) pi_stop(cx->c);
    backend_state_free(&cx->st);
    free(cx); free(b);
}
Backend *pi_backend_open(const backend_opts *opts) {
    pi_backend_ctx *cx = calloc(1, sizeof *cx);
    Backend *b = calloc(1, sizeof *b);
    if (!cx || !b) { free(cx); free(b); return NULL; }
    backend_state_init(&cx->st, opts);
    b->ctx = cx;
    b->caps = BACKEND_CAP_RESUME | BACKEND_CAP_EFFORT |
              BACKEND_CAP_LIVE_EFFORT;
    b->ask = pi_backend_ask;
    b->reset = pi_backend_reset;
    b->close = pi_backend_close;
    b->start = pi_backend_start;
    b->ask_ex = pi_backend_ask_ex;
    b->set_model = backend_set_model_generic;
    b->set_effort = pi_backend_set_effort;
    b->set_permission = backend_set_permission_none;
    b->set_event_cb = pi_backend_set_event_cb;
    b->set_abort_check = pi_backend_set_abort;
    b->session_id = pi_backend_session_id;
    b->model = backend_none;
    b->effort = backend_stored_effort;
    b->auth_source = backend_none;
    b->last_error = backend_none;
    return b;
}
#endif /* PI_BACKEND_IMPLEMENTATION */
