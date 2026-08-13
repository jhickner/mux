/*
 * grok.h — drive the Grok CLI (`grok agent stdio`) from C, mirroring claude.h.
 *
 * Spawns `grok agent stdio` as a persistent subprocess and speaks its ACP
 * (Agent Client Protocol, JSON-RPC 2.0 over stdio): initialize -> session/new
 * -> session/prompt, accumulating the streamed agent_message_chunk text into
 * the final reply. One process stays alive and retains context across turns.
 *
 *     #define GROK_IMPLEMENTATION      // in exactly one .c file
 *     #include "grok.h"
 *
 *     grok_opts o = {0};
 *     o.cwd = "/path/to/project";
 *     o.append_system = "You are a terse assistant.";  // prepended to each turn
 *     grok_client *c = grok_start(&o);
 *     char *reply = grok_send(c, "say hi");
 *     puts(reply); free(reply);
 *     grok_stop(c);
 *
 * Auth defaults to the cached token in ~/.grok/auth.json (your Grok
 * subscription) — no XAI_API_KEY needed. Depends on cJSON and a `grok` CLI on
 * PATH. ACP has no system-prompt field, so append_system is prepended to each
 * user turn (Grok caches the repeated prefix cheaply).
 */
#ifndef GROK_H
#define GROK_H

typedef struct grok_client grok_client;

typedef struct {
    const char *cli_path;        /* grok binary; NULL/"" -> "grok" (via PATH)   */
    const char *cwd;             /* child working directory; NULL -> inherit    */
    const char *model;           /* -m value (e.g. "grok-4.5"); NULL -> default */
    const char *reasoning_effort;/* --reasoning-effort low|medium|high; NULL    */
    const char *append_system;   /* prepended to each user turn; NULL -> none   */
} grok_opts;

/* Spawn a persistent `grok agent stdio` process. Returns NULL only on a local
 * setup failure: the ACP handshake (initialize + session/new) waits for the
 * first grok_send, so that the CLI's startup is not charged to the caller
 * before it has anything to ask. A missing or broken `grok` surfaces there. */
grok_client *grok_start(const grok_opts *opts);

/* Send one user turn (plain text) and return the final assistant text (malloc'd,
 * caller frees), or NULL on failure / process death. Blocks until the turn's
 * prompt response, and completes the handshake first when it is still pending.
 * Context is retained across calls by the CLI itself. */
char *grok_send(grok_client *c, const char *user_text);

/* The ACP session id, or NULL until the first turn has established one. */
const char *grok_session_id(grok_client *c);

/* When on, grok_send echoes a readable trace of the stream to stderr. */
void grok_set_verbose(grok_client *c, int on);

/* Per-event callback: `kind` is "assistant" | "thinking" | "tool"; `text` its
 * content. Runs on the grok_send thread. Pass NULL to clear. */
void grok_set_event_cb(grok_client *c,
                       void (*cb)(void *ud, const char *kind, const char *text),
                       void *ud);

/* Abort predicate; while it returns nonzero an in-flight grok_send stops
 * waiting and returns NULL. */
void grok_set_abort_check(grok_client *c, int (*cb)(void));

/* Terminate and reap the process, free the client. */
void grok_stop(grok_client *c);

#endif /* GROK_H */

/* ======================================================================== */
/*   IMPLEMENTATION                                                          */
/* ======================================================================== */
#ifdef GROK_IMPLEMENTATION

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

struct grok_client {
    pid_t pid;
    int   in_fd;
    int   out_fd;
    int  (*abort)(void);
    int   verbose;
    void (*on_event)(void *ud, const char *kind, const char *text);
    void *on_event_ud;
    char  session_id[128];
    char *cwd;                /* session/new working directory copy         */
    char *sys;                /* append_system copy, prepended to each turn */
    int   handshake_failed;   /* the deferred handshake was tried and lost  */
    int   next_id;            /* JSON-RPC request id counter                */
    char *buf;                /* line-assembly buffer for out_fd            */
    size_t len, cap;
};

void grok_set_verbose(grok_client *c, int on) { if (c) c->verbose = on; }
void grok_set_abort_check(grok_client *c, int (*cb)(void)) { if (c) c->abort = cb; }
void grok_set_event_cb(grok_client *c,
                       void (*cb)(void *ud, const char *kind, const char *text),
                       void *ud) {
    if (c) { c->on_event = cb; c->on_event_ud = ud; }
}
const char *grok_session_id(grok_client *c) {
    return (c && c->session_id[0]) ? c->session_id : NULL;
}

static void gk_sink(grok_client *c, const char *kind, const char *text) {
    if (!text) return;
    if (c->verbose)
        fprintf(stderr, "  [%s] %.400s%s\n", kind, text, strlen(text) > 400 ? " ..." : "");
    if (c->on_event) c->on_event(c->on_event_ud, kind, text);
}

/* Write one JSON-RPC object as a newline-delimited line. Frees `obj`. */
static int gk_write(grok_client *c, cJSON *obj) {
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json) return 0;
    size_t jl = strlen(json);
    char *withnl = realloc(json, jl + 2);
    if (!withnl) { free(json); return 0; }
    json = withnl; json[jl] = '\n'; json[jl + 1] = '\0';
    int ok = 1;
    for (size_t off = 0; off < jl + 1; ) {
        ssize_t w = write(c->in_fd, json + off, (jl + 1) - off);
        if (w < 0) { if (errno == EINTR) continue; ok = 0; break; }
        off += (size_t)w;
    }
    free(json);
    return ok;
}

/* Answer an agent->client request so the turn never blocks. We only expect
 * permission requests (our task uses no tools); approve the first allow-ish
 * option, else reply an empty result. */
static void gk_answer_request(grok_client *c, cJSON *req, cJSON *id) {
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(req, "method"));
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    cJSON *result = cJSON_AddObjectToObject(resp, "result");
    if (method && strcmp(method, "session/request_permission") == 0) {
        cJSON *params = cJSON_GetObjectItem(req, "params");
        cJSON *opts = params ? cJSON_GetObjectItem(params, "options") : NULL;
        const char *pick = NULL;
        cJSON *o;
        cJSON_ArrayForEach(o, opts) {
            const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(o, "kind"));
            const char *oid  = cJSON_GetStringValue(cJSON_GetObjectItem(o, "optionId"));
            if (!oid) continue;
            if (!pick) pick = oid;                       /* fallback: first */
            if (kind && strncmp(kind, "allow", 5) == 0) { pick = oid; break; }
        }
        cJSON *outcome = cJSON_AddObjectToObject(result, "outcome");
        cJSON_AddStringToObject(outcome, "outcome", "selected");
        cJSON_AddStringToObject(outcome, "optionId", pick ? pick : "allow");
    }
    gk_write(c, resp);
}

/* Handle one parsed event line during a wait for response `want_id`.
 * Accumulates agent_message_chunk text into *acc (if non-NULL). Returns:
 *   1  -> response for want_id arrived successfully; *ok set to result presence
 *   0  -> not the awaited response (keep reading)                              */
static int gk_handle(grok_client *c, cJSON *ev, int want_id, char **acc, int *ok) {
    cJSON *idj = cJSON_GetObjectItem(ev, "id");
    cJSON *method = cJSON_GetObjectItem(ev, "method");

    if (method && cJSON_IsString(method)) {
        if (idj) { gk_answer_request(c, ev, idj); return 0; }   /* agent request */
        /* notification */
        if (strcmp(method->valuestring, "session/update") == 0) {
            cJSON *params = cJSON_GetObjectItem(ev, "params");
            cJSON *u = params ? cJSON_GetObjectItem(params, "update") : NULL;
            const char *su = u ? cJSON_GetStringValue(cJSON_GetObjectItem(u, "sessionUpdate")) : NULL;
            if (su) {
                cJSON *content = cJSON_GetObjectItem(u, "content");
                const char *txt = content ? cJSON_GetStringValue(cJSON_GetObjectItem(content, "text")) : NULL;
                if (strcmp(su, "agent_message_chunk") == 0 && txt) {
                    if (acc) {
                        size_t al = *acc ? strlen(*acc) : 0, tl = strlen(txt);
                        char *n = realloc(*acc, al + tl + 1);
                        if (n) { memcpy(n + al, txt, tl + 1); *acc = n; }
                    }
                    gk_sink(c, "assistant", txt);
                } else if (strcmp(su, "agent_thought_chunk") == 0 && txt) {
                    gk_sink(c, "thinking", txt);
                }
            }
        }
        return 0;
    }

    /* response to one of our requests */
    if (idj && cJSON_IsNumber(idj) && (int)cJSON_GetNumberValue(idj) == want_id) {
        *ok = cJSON_GetObjectItem(ev, "result") != NULL;
        if (!*ok) {
            cJSON *err = cJSON_GetObjectItem(ev, "error");
            const char *m = err ? cJSON_GetStringValue(cJSON_GetObjectItem(err, "message")) : NULL;
            if (m) fprintf(stderr, "grok: rpc error: %s\n", m);
        }
        return 1;
    }
    return 0;
}

/* Read/parse lines until the response to `want_id`. On session/new we need the
 * result body, so `sid_out` (if non-NULL) is filled from result.sessionId.
 * Accumulates assistant text into *acc when non-NULL. Returns 1 on a successful
 * result, 0 on error/EOF/abort. */
static int gk_await(grok_client *c, int want_id, char **acc, char *sid_out, size_t sid_sz) {
    char tmp[8192];
    for (;;) {
        if (c->abort && c->abort()) { c->len = 0; return 0; }
        struct pollfd pfd = { c->out_fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, 200);
        if (pr < 0) { if (errno == EINTR) continue; c->len = 0; return 0; }
        if (pr == 0) continue;
        ssize_t r = read(c->out_fd, tmp, sizeof tmp);
        if (r <= 0) { c->len = 0; return 0; }

        if (c->len + (size_t)r + 1 > c->cap) {
            size_t nc = (c->len + (size_t)r + 1) * 2;
            char *nb = realloc(c->buf, nc);
            if (!nb) { c->len = 0; return 0; }
            c->buf = nb; c->cap = nc;
        }
        memcpy(c->buf + c->len, tmp, (size_t)r);
        c->len += (size_t)r;
        c->buf[c->len] = '\0';

        char *start = c->buf, *nl;
        while ((nl = memchr(start, '\n', c->len - (size_t)(start - c->buf)))) {
            *nl = '\0';
            cJSON *ev = cJSON_Parse(start);
            int done = 0, ok = 0;
            if (ev) {
                done = gk_handle(c, ev, want_id, acc, &ok);
                if (done && ok && sid_out) {
                    cJSON *res = cJSON_GetObjectItem(ev, "result");
                    const char *sid = res ? cJSON_GetStringValue(cJSON_GetObjectItem(res, "sessionId")) : NULL;
                    if (sid) snprintf(sid_out, sid_sz, "%s", sid);
                }
                cJSON_Delete(ev);
            }
            if (done) {
                size_t consumed = (size_t)(nl + 1 - c->buf);
                memmove(c->buf, c->buf + consumed, c->len - consumed);
                c->len -= consumed;
                return ok;
            }
            start = nl + 1;
        }
        size_t consumed = (size_t)(start - c->buf);
        if (consumed) { memmove(c->buf, start, c->len - consumed); c->len -= consumed; }
    }
}

grok_client *grok_start(const grok_opts *opts) {
    grok_opts o = opts ? *opts : (grok_opts){0};
    const char *cli = (o.cli_path && *o.cli_path) ? o.cli_path : "grok";

    signal(SIGPIPE, SIG_IGN);

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0) return NULL;
    if (pipe(out_pipe) != 0) { close(in_pipe[0]); close(in_pipe[1]); return NULL; }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return NULL;
    }
    if (pid == 0) {
        if (dup2(in_pipe[0], STDIN_FILENO) < 0)   _exit(126);
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0) _exit(126);
        if (in_pipe[0]  != STDIN_FILENO)  close(in_pipe[0]);
        if (in_pipe[1]  != STDIN_FILENO)  close(in_pipe[1]);
        if (out_pipe[0] != STDOUT_FILENO) close(out_pipe[0]);
        if (out_pipe[1] != STDOUT_FILENO) close(out_pipe[1]);
        if (o.cwd && *o.cwd) { if (chdir(o.cwd) != 0) _exit(126); }

        const char *argv[16];
        int n = 0;
        argv[n++] = cli;
        argv[n++] = "agent";
        if (o.model && *o.model)                     { argv[n++] = "-m"; argv[n++] = o.model; }
        if (o.reasoning_effort && *o.reasoning_effort) { argv[n++] = "--reasoning-effort"; argv[n++] = o.reasoning_effort; }
        argv[n++] = "--always-approve";   /* belt-and-suspenders with our permission handler */
        argv[n++] = "stdio";
        argv[n] = NULL;
        execvp(cli, (char *const *)argv);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    fcntl(in_pipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
    grok_client *c = calloc(1, sizeof *c);
    if (!c) {
        close(in_pipe[1]); close(out_pipe[0]);
        kill(pid, SIGKILL); waitpid(pid, NULL, 0);
        return NULL;
    }
    c->pid = pid;
    c->in_fd = in_pipe[1];
    c->out_fd = out_pipe[0];
    c->next_id = 1;
    c->cwd = (o.cwd && *o.cwd) ? strdup(o.cwd) : NULL;
    c->sys = (o.append_system && *o.append_system) ? strdup(o.append_system) : NULL;

    return c;
}

/* The ACP handshake, deferred to the first turn: it blocks until the CLI has
 * booted, which is over a second of Node startup that an interactive caller
 * would otherwise wait out before it could show anything. Returns nonzero once
 * the session is live. */
static int gk_handshake(grok_client *c) {
    if (c->session_id[0]) return 1;
    if (c->handshake_failed) return 0;
    c->handshake_failed = 1;

    cJSON *init = cJSON_CreateObject();
    cJSON_AddStringToObject(init, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(init, "id", c->next_id);
    cJSON_AddStringToObject(init, "method", "initialize");
    cJSON *ip = cJSON_AddObjectToObject(init, "params");
    cJSON_AddNumberToObject(ip, "protocolVersion", 1);
    cJSON_AddObjectToObject(ip, "clientCapabilities");
    if (!gk_write(c, init) || !gk_await(c, c->next_id, NULL, NULL, 0)) return 0;
    c->next_id++;

    cJSON *sn = cJSON_CreateObject();
    cJSON_AddStringToObject(sn, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(sn, "id", c->next_id);
    cJSON_AddStringToObject(sn, "method", "session/new");
    cJSON *sp = cJSON_AddObjectToObject(sn, "params");
    cJSON_AddStringToObject(sp, "cwd", c->cwd ? c->cwd : "/");
    cJSON_AddItemToObject(sp, "mcpServers", cJSON_CreateArray());
    if (!gk_write(c, sn) || !gk_await(c, c->next_id, NULL, c->session_id, sizeof c->session_id))
        return 0;
    c->next_id++;

    c->handshake_failed = 0;
    return 1;
}

char *grok_send(grok_client *c, const char *user_text) {
    if (!c || !user_text || !gk_handshake(c)) return NULL;

    /* Prepend the system prompt to each turn (ACP has no system field). */
    char *full = NULL;
    if (c->sys) {
        size_t need = strlen(c->sys) + strlen(user_text) + 8;
        full = malloc(need);
        if (!full) return NULL;
        snprintf(full, need, "%s\n\n---\n\n%s", c->sys, user_text);
    }
    const char *payload = full ? full : user_text;

    int id = c->next_id++;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", id);
    cJSON_AddStringToObject(req, "method", "session/prompt");
    cJSON *p = cJSON_AddObjectToObject(req, "params");
    cJSON_AddStringToObject(p, "sessionId", c->session_id);
    cJSON *arr = cJSON_AddArrayToObject(p, "prompt");
    cJSON *block = cJSON_CreateObject();
    cJSON_AddStringToObject(block, "type", "text");
    cJSON_AddStringToObject(block, "text", payload);
    cJSON_AddItemToArray(arr, block);
    int wrote = gk_write(c, req);
    free(full);
    if (!wrote) return NULL;

    char *acc = NULL;
    int ok = gk_await(c, id, &acc, NULL, 0);
    if (!ok) { free(acc); return NULL; }
    return acc ? acc : strdup("");
}

/* Poll for the child's exit for up to `ms`. Returns nonzero once reaped. */
static int gk_reap_within(grok_client *c, int ms) {
    for (int waited = 0; waited < ms; waited += 5) {
        if (waitpid(c->pid, NULL, WNOHANG) == c->pid) { c->pid = 0; return 1; }
        usleep(5000);
    }
    return 0;
}

void grok_stop(grok_client *c) {
    if (!c) return;
    if (c->in_fd >= 0) close(c->in_fd);     /* EOF on stdin asks it to exit */
    if (c->pid > 0) {
        /* We only stop between turns, so the CLI has nothing left to finish.
         * Waiting out the Node runtime's unwind costs about a second, which an
         * interactive caller feels on quit, so escalate immediately. */
        kill(c->pid, SIGTERM);
        if (!gk_reap_within(c, 50)) {
            kill(c->pid, SIGKILL);
            waitpid(c->pid, NULL, 0);
            c->pid = 0;
        }
    }
    if (c->out_fd >= 0) close(c->out_fd);
    free(c->cwd);
    free(c->sys);
    free(c->buf);
    free(c);
}

#endif /* GROK_IMPLEMENTATION */
