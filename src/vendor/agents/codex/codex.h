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
    const char *sandbox;        /* read-only|workspace-write|danger-full-access;
                                   NULL -> "workspace-write"                  */
    const char *resume_session; /* resume this thread instead of starting one */
    const char *append_system;  /* thread developer instructions; NULL -> none */
    int bypass_approvals;       /* danger-full-access, only if externally sandboxed */
    int skip_git_repo_check;    /* retained for source compatibility; unused  */
} codex_opts;

/* Spawn app-server, initialize it, and start/resume a thread. */
codex_client *codex_start(const codex_opts *opts);

/* Start one turn on the existing process/thread and return its final agent
 * message (malloc'd), or NULL on failure/abort. */
char *codex_send(codex_client *c, const char *user_text);

const char *codex_session_id(codex_client *c);

/* Make the next send start a fresh thread on the same app-server process. */
void codex_reset(codex_client *c);

void codex_set_verbose(codex_client *c, int on);
void codex_set_event_cb(codex_client *c,
                        void (*cb)(void *ud, const char *kind, const char *text),
                        void *ud);
void codex_set_abort_check(codex_client *c, int (*cb)(void));
void codex_stop(codex_client *c);

#endif /* CODEX_H */

#ifdef CODEX_IMPLEMENTATION

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "cJSON.h"

struct codex_client {
    pid_t pid;
    int in_fd, out_fd, next_id, verbose;
    int (*abort)(void);
    void (*on_event)(void *ud, const char *kind, const char *text);
    void *on_event_ud;
    char *model, *sandbox, *sys;
    char session_id[128];
    char *buf;
    size_t len, cap;
};

static char *cx_dup(const char *s) { return (s && *s) ? strdup(s) : NULL; }

static void cx_sink(codex_client *c, const char *kind, const char *text) {
    if (!text) return;
    if (c->verbose)
        fprintf(stderr, "  [%s] %.400s%s\n", kind, text,
                strlen(text) > 400 ? " ..." : "");
    if (c->on_event) c->on_event(c->on_event_ud, kind, text);
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
        int pr = poll(&p, 1, 200);
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
    if (resume && *resume) {
        cJSON_AddStringToObject(p, "threadId", resume);
    } else {
        if (c->model) cJSON_AddStringToObject(p, "model", c->model);
        if (c->sys) cJSON_AddStringToObject(p, "developerInstructions", c->sys);
        cJSON_AddStringToObject(p, "approvalPolicy", "never");
        cJSON_AddStringToObject(p, "sandbox", c->sandbox);
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

codex_client *codex_start(const codex_opts *opts) {
    codex_opts o = opts ? *opts : (codex_opts){0};
    const char *cli = o.cli_path && *o.cli_path ? o.cli_path : "codex";
    codex_client *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->in_fd = c->out_fd = -1; c->next_id = 1;
    c->model = cx_dup(o.model); c->sys = cx_dup(o.append_system);
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
    if (!cx_initialize(c) || !cx_open_thread(c, o.resume_session)) {
        codex_stop(c); return NULL;
    }
    return c;
}

void codex_set_verbose(codex_client *c, int on) { if (c) c->verbose = on; }
void codex_set_abort_check(codex_client *c, int (*cb)(void)) { if (c) c->abort = cb; }
void codex_set_event_cb(codex_client *c,
                        void (*cb)(void *ud, const char *kind, const char *text),
                        void *ud) {
    if (c) { c->on_event = cb; c->on_event_ud = ud; }
}
const char *codex_session_id(codex_client *c) {
    return c && c->session_id[0] ? c->session_id : NULL;
}
void codex_reset(codex_client *c) { if (c) c->session_id[0] = '\0'; }

static void cx_item_event(codex_client *c, cJSON *params, char **fallback) {
    cJSON *item = params ? cJSON_GetObjectItemCaseSensitive(params, "item") : NULL;
    const char *type = item ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(item, "type")) : NULL;
    if (!type) return;
    if (!strcmp(type, "agentMessage")) {
        const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "text"));
        if (s) { free(*fallback); *fallback = strdup(s); }
    } else if (!strcmp(type, "reasoning")) {
        cJSON *summary = cJSON_GetObjectItemCaseSensitive(item, "summary");
        cJSON *s = cJSON_IsArray(summary) ? cJSON_GetArrayItem(summary, 0) : NULL;
        if (cJSON_IsString(s)) cx_sink(c, "thinking", s->valuestring);
    } else if (!strcmp(type, "commandExecution")) {
        const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "command"));
        const char *out = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "aggregatedOutput"));
        if (cmd) cx_sink(c, "tool", cmd);
        if (out) cx_sink(c, "tool-result", out);
    }
}

char *codex_send(codex_client *c, const char *user_text) {
    if (!c || !user_text) return NULL;
    if (!c->session_id[0] && !cx_open_thread(c, NULL)) return NULL;
    cJSON *p = cJSON_CreateObject(), *input = cJSON_CreateArray();
    cJSON *text = cJSON_CreateObject();
    cJSON_AddStringToObject(text, "type", "text");
    cJSON_AddStringToObject(text, "text", user_text);
    cJSON_AddItemToArray(input, text);
    cJSON_AddStringToObject(p, "threadId", c->session_id);
    cJSON_AddItemToObject(p, "input", input);
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
            interrupted = 1; continue;
        }
        if (rr != 1) { failed = 1; break; }
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(msg, "id");
        const char *method = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(msg, "method"));
        cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
        if (jid && method) {
            cx_reject_server_request(c, msg);
        } else if (method && !strcmp(method, "item/agentMessage/delta")) {
            const char *d = params ? cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(params, "delta")) : NULL;
            if (d) { cx_append(&answer, d); cx_sink(c, "assistant", d); }
        } else if (method && !strcmp(method, "item/completed")) {
            cx_item_event(c, params, &fallback);
        } else if (method && !strcmp(method, "turn/completed")) {
            cJSON *t = params ? cJSON_GetObjectItemCaseSensitive(params, "turn") : NULL;
            const char *status = t ? cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(t, "status")) : NULL;
            if (!status || strcmp(status, "completed")) failed = 1;
            completed = 1;
        }
        cJSON_Delete(msg);
    }
    if (!answer && fallback) { answer = fallback; fallback = NULL; }
    free(fallback);
    if (failed || interrupted || !completed) { free(answer); return NULL; }
    return answer ? answer : strdup("");
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
    if (c->out_fd >= 0) close(c->out_fd);
    free(c->model); free(c->sandbox); free(c->sys); free(c->buf); free(c);
}

#endif /* CODEX_IMPLEMENTATION */
