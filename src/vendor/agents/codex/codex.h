/*
 * codex.h — drive one long-lived Codex app-server process from C.
 *
 * The child speaks Codex's JSONL app-server protocol over stdin/stdout. A
 * single process owns the thread for its entire lifetime; codex_send() starts
 * additional turns in that thread without spawning another CLI process.
 *
 *     #define CODEX_IMPLEMENTATION
 *     #include "codex.h"
 *
 *     codex_opts o = { .cwd = "/path/to/project" };
 *     codex_client *c = codex_start(&o);
 *     char *reply = codex_send(c, "inspect the failing tests");
 *     puts(reply); free(reply);
 *     codex_stop(c);
 *
 * Depends on cJSON and a logged-in `codex` CLI with `app-server` support.
 */
#ifndef CODEX_H
#define CODEX_H

typedef struct codex_client codex_client;

typedef struct {
    const char *cli_path;       /* codex binary; NULL/"" -> "codex" via PATH */
    const char *cwd;            /* child working directory; NULL -> inherit   */
    const char *model;          /* thread model; NULL -> configured default   */
    const char *effort;         /* turn effort; NULL -> configured default    */
    const char *sandbox;        /* read-only|workspace-write|danger-full-access;
                                   NULL -> "workspace-write"                  */
    const char *resume_session; /* resume this thread instead of starting one */
    const char *append_system;  /* thread developer instructions; NULL -> none */
    int bypass_approvals;       /* danger-full-access, only if externally sandboxed */
    int skip_git_repo_check;    /* retained for source compatibility; unused  */
} codex_opts;

/* Spawn app-server and initialize/start its thread in the background. Returns
 * once the child and worker exist; the first operation waits if startup is
 * still in progress. */
codex_client *codex_start(const codex_opts *opts);

/* Start one turn on the existing process/thread and return its final agent
 * message (malloc'd), or NULL on failure. An interrupted turn returns the text
 * produced so far (possibly empty), leaving the process ready for another
 * turn. */
char *codex_send(codex_client *c, const char *user_text);

typedef struct {
    int interrupted;   /* the abort predicate ended the turn */
    long context_tokens; /* latest model request, including output */
    long context_window;
} codex_result;

/* As codex_send, but also fills *meta (zeroed first). `meta` may be NULL. */
char *codex_send_ex(codex_client *c, const char *user_text, codex_result *meta);

const char *codex_session_id(codex_client *c);

/* Make the next send start a fresh thread on the same app-server process. */
void codex_reset(codex_client *c);

void codex_set_verbose(codex_client *c, int on);

/* One display-worthy app-server event. Tool starts preserve the structured
 * input Codex reports; a completed fileChange also carries its authoritative
 * unified diff. All strings are borrowed for the callback. */
typedef enum {
    CODEX_EV_ASSISTANT,
    CODEX_EV_THINKING,
    CODEX_EV_TOOL,
    CODEX_EV_TOOL_RESULT,
} codex_event_kind;

typedef struct {
    codex_event_kind kind;
    const char *text;       /* assistant/thinking text, or tool output */
    const char *name;       /* tool name, for CODEX_EV_TOOL           */
    const char *input_json; /* compact tool input, for CODEX_EV_TOOL  */
    const char *diff;       /* unified patch, for a file-change result */
} codex_event;

void codex_set_event_cb(codex_client *c,
                        void (*cb)(void *ud, const codex_event *ev),
                        void *ud);
void codex_set_abort_check(codex_client *c, int (*cb)(void));
/* Override subsequent turns' reasoning effort. NULL clears the override. */
int codex_set_effort(codex_client *c, const char *effort);
/* Latest context occupancy reported by thread/tokenUsage/updated. */
void codex_usage(codex_client *c, long *context_tokens, long *context_window);
void codex_stop(codex_client *c);

#endif /* CODEX_H */

#ifdef CODEX_IMPLEMENTATION

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "cJSON.h"

/* How long a read waits on the app-server before running the abort predicate
 * again. Also the worst-case delay before a key the caller reads from that
 * predicate reaches the screen, so it sits below the ~30ms an echo can take
 * without being felt as lag. */
#define CX_TICK_MS 20

struct codex_client {
    pid_t pid;
    int in_fd, out_fd, next_id, verbose;
    int (*abort)(void);
    void (*on_event)(void *ud, const codex_event *ev);
    void *on_event_ud;
    char *model, *effort, *sandbox, *sys, *resume;
    int effort_changed;
    char session_id[128];
    long context_tokens, context_window;
    char *buf;
    size_t len, cap;
    pthread_t warm_thread;
    int warm_joinable;
    atomic_int warm_state;     /* 0 while starting, 1 ready, -1 failed */
};

static char *cx_dup(const char *s) { return (s && *s) ? strdup(s) : NULL; }

static void cx_emit(codex_client *c, const codex_event *ev) {
    static const char *const kinds[] = {
        "assistant", "thinking", "tool", "tool-result"
    };
    const char *preview = ev->kind == CODEX_EV_TOOL ? ev->name : ev->text;
    if (!preview) preview = ev->diff ? ev->diff : "";
    if (c->verbose) {
        fprintf(stderr, "  [%s] %.400s%s\n", kinds[ev->kind], preview,
                strlen(preview) > 400 ? " ..." : "");
    }
    if (c->on_event) c->on_event(c->on_event_ud, ev);
}

static void cx_text_event(codex_client *c, codex_event_kind kind, const char *text) {
    if (!text) return;
    codex_event ev = { .kind = kind, .text = text };
    cx_emit(c, &ev);
}

static void cx_append(char **dst, const char *s) {
    if (!s) return;
    size_t a = *dst ? strlen(*dst) : 0, b = strlen(s);
    char *n = realloc(*dst, a + b + 1);
    if (!n) return;
    memcpy(n + a, s, b + 1); *dst = n;
}

/* Write one compact JSON object followed by a newline. Takes ownership. */
static int cx_write(codex_client *c, cJSON *obj) {
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) return 0;
    size_t n = strlen(s), off = 0;
    char *line = realloc(s, n + 2);
    if (!line) { free(s); return 0; }
    line[n++] = '\n';
    while (off < n) {
        ssize_t w = write(c->in_fd, line + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; free(line); return 0; }
        off += (size_t)w;
    }
    free(line); return 1;
}

static int cx_request(codex_client *c, const char *method, cJSON *params) {
    cJSON *o = cJSON_CreateObject();
    int id = c->next_id++;
    cJSON_AddNumberToObject(o, "id", id);
    cJSON_AddStringToObject(o, "method", method);
    cJSON_AddItemToObject(o, "params", params ? params : cJSON_CreateObject());
    return cx_write(c, o) ? id : 0;
}

/* Read one JSONL message. Return 1/message, 0/abort predicate, -1/error. */
static int cx_read(codex_client *c, cJSON **out, int honor_abort) {
    *out = NULL;
    for (;;) {
        char *nl = memchr(c->buf, '\n', c->len);
        if (nl) {
            size_t n = (size_t)(nl - c->buf);
            if (n && c->buf[n - 1] == '\r') n--;
            char save = c->buf[n]; c->buf[n] = '\0';
            *out = n ? cJSON_Parse(c->buf) : NULL;
            c->buf[n] = save;
            size_t used = (size_t)(nl + 1 - c->buf);
            memmove(c->buf, c->buf + used, c->len - used); c->len -= used;
            if (*out) return 1;
            continue;
        }
        if (honor_abort && c->abort && c->abort()) return 0;
        struct pollfd p = { c->out_fd, POLLIN, 0 };
        int pr = poll(&p, 1, CX_TICK_MS);
        if (pr < 0) { if (errno == EINTR) continue; return -1; }
        if (!pr) continue;
        char tmp[8192]; ssize_t nr = read(c->out_fd, tmp, sizeof tmp);
        if (nr <= 0) return -1;
        if (c->len + (size_t)nr + 1 > c->cap) {
            size_t nc = (c->len + (size_t)nr + 1) * 2;
            char *nb = realloc(c->buf, nc);
            if (!nb) return -1;
            c->buf = nb; c->cap = nc;
        }
        memcpy(c->buf + c->len, tmp, (size_t)nr); c->len += (size_t)nr;
    }
}

/* App-server can issue client requests. Headless threads use approvalPolicy
 * never, so reject any unexpected interactive request rather than deadlock. */
static void cx_reject_server_request(codex_client *c, cJSON *msg) {
    cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    cJSON *method = cJSON_GetObjectItemCaseSensitive(msg, "method");
    if (!id || !cJSON_IsString(method)) return;
    cJSON *o = cJSON_CreateObject(), *err = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "id", cJSON_Duplicate(id, 1));
    cJSON_AddNumberToObject(err, "code", -32601);
    cJSON_AddStringToObject(err, "message", "interactive request unsupported");
    cJSON_AddItemToObject(o, "error", err); cx_write(c, o);
}

static cJSON *cx_wait_response(codex_client *c, int want) {
    for (;;) {
        cJSON *msg;
        if (cx_read(c, &msg, 0) != 1) return NULL;
        cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
        cJSON *method = cJSON_GetObjectItemCaseSensitive(msg, "method");
        if (id && cJSON_IsNumber(id) && id->valueint == want && !method) {
            if (cJSON_GetObjectItemCaseSensitive(msg, "error")) {
                cJSON_Delete(msg); return NULL;
            }
            return msg;
        }
        if (id && method) cx_reject_server_request(c, msg);
        cJSON_Delete(msg);
    }
}

static int cx_initialize(codex_client *c) {
    cJSON *p = cJSON_CreateObject(), *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", "codex.h");
    cJSON_AddStringToObject(info, "version", "1");
    cJSON_AddItemToObject(p, "clientInfo", info);
    int id = cx_request(c, "initialize", p);
    cJSON *r = id ? cx_wait_response(c, id) : NULL;
    if (!r) return 0;
    cJSON_Delete(r);
    cJSON *note = cJSON_CreateObject();
    cJSON_AddStringToObject(note, "method", "initialized");
    return cx_write(c, note);
}

static int cx_open_thread(codex_client *c, const char *resume) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "approvalPolicy", "never");
    cJSON_AddStringToObject(p, "sandbox", c->sandbox);
    if (resume && *resume) {
        cJSON_AddStringToObject(p, "threadId", resume);
    } else {
        if (c->model) cJSON_AddStringToObject(p, "model", c->model);
        if (c->sys) cJSON_AddStringToObject(p, "developerInstructions", c->sys);
    }
    int id = cx_request(c, resume && *resume ? "thread/resume" : "thread/start", p);
    cJSON *r = id ? cx_wait_response(c, id) : NULL;
    if (!r) return 0;
    cJSON *result = cJSON_GetObjectItemCaseSensitive(r, "result");
    cJSON *thread = result ? cJSON_GetObjectItemCaseSensitive(result, "thread") : NULL;
    const char *sid = thread ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(thread, "id")) : NULL;
    if (sid) snprintf(c->session_id, sizeof c->session_id, "%s", sid);
    cJSON_Delete(r); return sid != NULL;
}

/* The app-server handshake and thread creation are local but noticeably slower
 * than drawing the prompt. Do them while the user is reading or typing instead
 * of holding the whole interface behind them. */
static void *cx_warm(void *arg) {
    codex_client *c = arg;
    int ok = cx_initialize(c) && cx_open_thread(c, c->resume);
    atomic_store_explicit(&c->warm_state, ok ? 1 : -1, memory_order_release);
    return NULL;
}

/* A turn submitted unusually quickly waits for the one startup worker. In the
 * normal interactive case it has already finished by the time this is called. */
static int cx_await_ready(codex_client *c) {
    if (!c) return 0;
    if (c->warm_joinable) {
        pthread_join(c->warm_thread, NULL);
        c->warm_joinable = 0;
    }
    return atomic_load_explicit(&c->warm_state, memory_order_acquire) == 1;
}

codex_client *codex_start(const codex_opts *opts) {
    codex_opts o = opts ? *opts : (codex_opts){0};
    const char *cli = o.cli_path && *o.cli_path ? o.cli_path : "codex";
    codex_client *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->in_fd = c->out_fd = -1; c->next_id = 1;
    c->model = cx_dup(o.model); c->effort = cx_dup(o.effort);
    c->sys = cx_dup(o.append_system);
    c->resume = cx_dup(o.resume_session);
    c->sandbox = strdup(o.bypass_approvals ? "danger-full-access" :
                        (o.sandbox && *o.sandbox ? o.sandbox : "workspace-write"));
    if (!c->sandbox) { codex_stop(c); return NULL; }
    int in[2], out[2];
    if (pipe(in)) { codex_stop(c); return NULL; }
    if (pipe(out)) { close(in[0]); close(in[1]); codex_stop(c); return NULL; }
    signal(SIGPIPE, SIG_IGN);
    pid_t pid = fork();
    if (pid < 0) {
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        codex_stop(c); return NULL;
    }
    if (!pid) {
        if (dup2(in[0], STDIN_FILENO) < 0 || dup2(out[1], STDOUT_FILENO) < 0)
            _exit(126);
        if (in[0] != STDIN_FILENO) close(in[0]);
        if (in[1] != STDIN_FILENO) close(in[1]);
        if (out[0] != STDOUT_FILENO) close(out[0]);
        if (out[1] != STDOUT_FILENO) close(out[1]);
        if (o.cwd && *o.cwd && chdir(o.cwd)) _exit(126);
        const char *argv[] = {
            cli, "app-server", "--stdio",
            "--disable", "hooks",
            "--disable", "apps",
            "--disable", "remote_plugin",
            "--disable", "multi_agent",
            "--config", "project_doc_max_bytes=0",
            NULL
        };
        execvp(cli, (char *const *)argv); _exit(127);
    }
    close(in[0]); close(out[1]);
    c->pid = pid; c->in_fd = in[1]; c->out_fd = out[0];
    atomic_init(&c->warm_state, 0);
    if (!pthread_create(&c->warm_thread, NULL, cx_warm, c))
        c->warm_joinable = 1;
    else
        cx_warm(c);             /* rare resource failure: retain old semantics */
    return c;
}

void codex_set_verbose(codex_client *c, int on) { if (c) c->verbose = on; }
void codex_set_abort_check(codex_client *c, int (*cb)(void)) { if (c) c->abort = cb; }
int codex_set_effort(codex_client *c, const char *effort) {
    if (!c) return 0;
    free(c->effort);
    c->effort = cx_dup(effort);
    c->effort_changed = 1;
    return 1;
}
void codex_usage(codex_client *c, long *tokens, long *window) {
    if (tokens) *tokens = c ? c->context_tokens : 0;
    if (window) *window = c ? c->context_window : 0;
}
void codex_set_event_cb(codex_client *c,
                        void (*cb)(void *ud, const codex_event *ev),
                        void *ud) {
    if (c) { c->on_event = cb; c->on_event_ud = ud; }
}
const char *codex_session_id(codex_client *c) {
    if (!c || atomic_load_explicit(&c->warm_state, memory_order_acquire) != 1)
        return NULL;
    return c->session_id[0] ? c->session_id : NULL;
}
void codex_reset(codex_client *c) {
    if (c) {
        if (!cx_await_ready(c)) return;
        c->session_id[0] = '\0';
        c->context_tokens = c->context_window = 0;
    }
}

/* `total` accumulates every request in the thread. `last` is the most recent
 * model request and therefore the amount occupying the context window. */
static void cx_note_usage(codex_client *c, cJSON *params) {
    cJSON *usage = params ? cJSON_GetObjectItemCaseSensitive(params, "tokenUsage") : NULL;
    cJSON *last = usage ? cJSON_GetObjectItemCaseSensitive(usage, "last") : NULL;
    cJSON *used = last ? cJSON_GetObjectItemCaseSensitive(last, "totalTokens") : NULL;
    cJSON *window = usage ? cJSON_GetObjectItemCaseSensitive(usage, "modelContextWindow") : NULL;
    if (cJSON_IsNumber(used) && used->valuedouble > 0)
        c->context_tokens = (long)used->valuedouble;
    if (cJSON_IsNumber(window) && window->valuedouble > 0)
        c->context_window = (long)window->valuedouble;
}

/* The generic renderer expects a command under `command`, and a file edit's
 * first path under `file_path`. Keep the full changes array too: callers that
 * understand Codex can inspect every path and change kind. */
static char *cx_command_input(cJSON *item) {
    const char *command = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(item, "command"));
    const char *cwd = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(item, "cwd"));
    if (!command) return NULL;
    cJSON *input = cJSON_CreateObject();
    cJSON_AddStringToObject(input, "command", command);
    if (cwd) cJSON_AddStringToObject(input, "cwd", cwd);
    char *json = cJSON_PrintUnformatted(input);
    cJSON_Delete(input);
    return json;
}

static char *cx_file_input(cJSON *changes) {
    if (!cJSON_IsArray(changes)) return NULL;
    cJSON *input = cJSON_CreateObject();
    cJSON *first = cJSON_GetArrayItem(changes, 0);
    const char *path = first ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(first, "path")) : NULL;
    if (path) cJSON_AddStringToObject(input, "file_path", path);
    cJSON *copy = cJSON_Duplicate(changes, 1);
    if (copy) cJSON_AddItemToObject(input, "changes", copy);
    char *json = cJSON_PrintUnformatted(input);
    cJSON_Delete(input);
    return json;
}

/* Prefix each per-file hunk with a private marker consumed by filediff. The
 * app-server's diff intentionally contains hunks rather than filename headers. */
static char *cx_file_diff(cJSON *changes) {
    if (!cJSON_IsArray(changes)) return NULL;
    char *patch = NULL;
    int n = cJSON_GetArraySize(changes);
    for (int i = 0; i < n; i++) {
        cJSON *change = cJSON_GetArrayItem(changes, i);
        const char *path = change ? cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(change, "path")) : NULL;
        const char *diff = change ? cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(change, "diff")) : NULL;
        if (!path || !diff) continue;
        cx_append(&patch, "@@file ");
        cx_append(&patch, path);
        cx_append(&patch, "\n");
        cx_append(&patch, diff);
        size_t len = strlen(diff);
        if (!len || diff[len - 1] != '\n') cx_append(&patch, "\n");
    }
    return patch;
}

static void cx_item_event(codex_client *c, cJSON *params, int started,
                          char **fallback) {
    cJSON *item = params ? cJSON_GetObjectItemCaseSensitive(params, "item") : NULL;
    const char *type = item ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(item, "type")) : NULL;
    if (!type) return;
    if (!strcmp(type, "agentMessage")) {
        if (started) return;
        const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "text"));
        if (s) { free(*fallback); *fallback = strdup(s); }
    } else if (!strcmp(type, "reasoning")) {
        if (started) return;
        cJSON *summary = cJSON_GetObjectItemCaseSensitive(item, "summary");
        cJSON *s = cJSON_IsArray(summary) ? cJSON_GetArrayItem(summary, 0) : NULL;
        if (cJSON_IsString(s)) cx_text_event(c, CODEX_EV_THINKING, s->valuestring);
    } else if (!strcmp(type, "commandExecution")) {
        if (started) {
            char *input = cx_command_input(item);
            codex_event ev = {
                .kind = CODEX_EV_TOOL,
                .name = "Shell",
                .input_json = input,
            };
            cx_emit(c, &ev);
            free(input);
        } else {
            const char *out = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(item, "aggregatedOutput"));
            const char *status = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(item, "status"));
            codex_event ev = {
                .kind = CODEX_EV_TOOL_RESULT,
                .text = out ? out : (status && strcmp(status, "completed") ? status : ""),
            };
            cx_emit(c, &ev);
        }
    } else if (!strcmp(type, "fileChange")) {
        cJSON *changes = cJSON_GetObjectItemCaseSensitive(item, "changes");
        if (started) {
            char *input = cx_file_input(changes);
            codex_event ev = {
                .kind = CODEX_EV_TOOL,
                .name = "Edit",
                .input_json = input,
            };
            cx_emit(c, &ev);
            free(input);
        } else {
            const char *status = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(item, "status"));
            int completed = status && !strcmp(status, "completed");
            char *diff = completed ? cx_file_diff(changes) : NULL;
            codex_event ev = {
                .kind = CODEX_EV_TOOL_RESULT,
                .text = completed ? "" : (status ? status : "failed"),
                .diff = diff,
            };
            cx_emit(c, &ev);
            free(diff);
        }
    }
}

char *codex_send_ex(codex_client *c, const char *user_text, codex_result *meta) {
    if (meta) memset(meta, 0, sizeof *meta);
    if (!c || !user_text) return NULL;
    if (!cx_await_ready(c)) return NULL;
    if (!c->session_id[0] && !cx_open_thread(c, NULL)) return NULL;
    cJSON *p = cJSON_CreateObject(), *input = cJSON_CreateArray();
    cJSON *text = cJSON_CreateObject();
    cJSON_AddStringToObject(text, "type", "text");
    cJSON_AddStringToObject(text, "text", user_text);
    cJSON_AddItemToArray(input, text);
    cJSON_AddStringToObject(p, "threadId", c->session_id);
    cJSON_AddItemToObject(p, "input", input);
    if (c->effort)
        cJSON_AddStringToObject(p, "effort", c->effort);
    else if (c->effort_changed)
        cJSON_AddNullToObject(p, "effort");
    int id = cx_request(c, "turn/start", p);
    cJSON *response = id ? cx_wait_response(c, id) : NULL;
    if (!response) return NULL;
    cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
    cJSON *turn = result ? cJSON_GetObjectItemCaseSensitive(result, "turn") : NULL;
    const char *tid = turn ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(turn, "id")) : NULL;
    char turn_id[128] = "";
    if (tid) snprintf(turn_id, sizeof turn_id, "%s", tid);
    cJSON_Delete(response);
    if (!turn_id[0]) return NULL;
    c->effort_changed = 0;

    char *answer = NULL, *fallback = NULL;
    int completed = 0, failed = 0, interrupted = 0;
    while (!completed) {
        cJSON *msg;
        int rr = cx_read(c, &msg, !interrupted);
        if (rr == 0 && !interrupted) {
            cJSON *ip = cJSON_CreateObject();
            cJSON_AddStringToObject(ip, "threadId", c->session_id);
            cJSON_AddStringToObject(ip, "turnId", turn_id);
            int iid = cx_request(c, "turn/interrupt", ip);
            if (!iid) { failed = 1; break; }
            if (meta) meta->interrupted = 1;
            interrupted = 1; continue;
        }
        if (rr != 1) { failed = 1; break; }
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(msg, "id");
        const char *method = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(msg, "method"));
        cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
        if (jid && method) {
            cx_reject_server_request(c, msg);
        } else if (method && !strcmp(method, "thread/tokenUsage/updated")) {
            cx_note_usage(c, params);
        } else if (method && !strcmp(method, "item/agentMessage/delta")) {
            const char *d = params ? cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(params, "delta")) : NULL;
            if (d) { cx_append(&answer, d); cx_text_event(c, CODEX_EV_ASSISTANT, d); }
        } else if (method && !strcmp(method, "item/started")) {
            cx_item_event(c, params, 1, &fallback);
        } else if (method && !strcmp(method, "item/completed")) {
            cx_item_event(c, params, 0, &fallback);
        } else if (method && !strcmp(method, "turn/completed")) {
            cJSON *t = params ? cJSON_GetObjectItemCaseSensitive(params, "turn") : NULL;
            const char *status = t ? cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(t, "status")) : NULL;
            if (status && !strcmp(status, "interrupted")) {
                if (meta) meta->interrupted = 1;
            } else if (!status || strcmp(status, "completed")) {
                failed = 1;
            }
            completed = 1;
        }
        cJSON_Delete(msg);
    }
    if (!answer && fallback) { answer = fallback; fallback = NULL; }
    free(fallback);
    if (failed || !completed) { free(answer); return NULL; }
    if (meta) {
        meta->context_tokens = c->context_tokens;
        meta->context_window = c->context_window;
    }
    return answer ? answer : strdup("");
}

char *codex_send(codex_client *c, const char *user_text) {
    return codex_send_ex(c, user_text, NULL);
}

/* Poll for the child's exit for up to `ms`. Returns nonzero once reaped. */
static int cx_reap_within(codex_client *c, int ms) {
    for (int waited = 0; waited < ms; waited += 5) {
        if (waitpid(c->pid, NULL, WNOHANG) == c->pid) { c->pid = 0; return 1; }
        usleep(5000);
    }
    return 0;
}

void codex_stop(codex_client *c) {
    if (!c) return;
    if (c->in_fd >= 0) close(c->in_fd);     /* EOF on stdin asks it to exit */
    if (c->pid > 0) {
        /* We only stop between turns, so the app-server has nothing left to
         * finish. Waiting out its unwind costs about a second, which an
         * interactive caller feels on quit, so escalate immediately. */
        kill(c->pid, SIGTERM);
        if (!cx_reap_within(c, 50)) {
            kill(c->pid, SIGKILL);
            waitpid(c->pid, NULL, 0);
            c->pid = 0;
        }
    }
    if (c->warm_joinable) {
        pthread_join(c->warm_thread, NULL);
        c->warm_joinable = 0;
    }
    if (c->out_fd >= 0) close(c->out_fd);
    free(c->model); free(c->effort); free(c->sandbox); free(c->sys); free(c->resume);
    free(c->buf); free(c);
}

#endif /* CODEX_IMPLEMENTATION */
