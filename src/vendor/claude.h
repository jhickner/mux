/*
 * claude.h — drive the Claude Code CLI (`claude`) from C, the way the Claude
 * Agent SDK does: spawn it in headless stream-JSON mode as a persistent
 * subprocess and exchange newline-delimited JSON over its stdin/stdout.
 *
 * You get the full Claude Code harness — every built-in tool, every skill,
 * subagents, context management — driven from your own program. One process
 * stays alive and retains conversation context across turns.
 *
 *     #define CLAUDE_IMPLEMENTATION      // in exactly one .c file
 *     #include "claude.h"
 *
 *     claude_opts o = {0};
 *     o.cwd = "/path/to/project";
 *     o.permission_mode = "bypassPermissions";  // run tools without prompts
 *     o.use_subscription = 1;                    // unset ANTHROPIC_API_KEY -> claude.ai login
 *     claude_client *c = claude_start(&o);
 *     char *reply = claude_send(c, "what files are here?");
 *     puts(reply); free(reply);
 *     claude_stop(c);
 *
 * Depends on cJSON (bundled beside this header) and a `claude` CLI on PATH
 * (`npm i -g @anthropic-ai/claude-code`, or point cli_path at it).
 */
#ifndef CLAUDE_H
#define CLAUDE_H

typedef struct claude_client claude_client;

typedef struct {
    const char *cli_path;        /* claude binary; NULL/"" -> "claude" (via PATH) */
    const char *cwd;             /* child working directory; NULL -> inherit      */
    const char *model;           /* --model value; NULL -> CLI default            */
    const char *permission_mode; /* --permission-mode (e.g. "bypassPermissions")  */
    const char *resume_session;  /* --resume <id> to continue a prior session     */
    const char *append_system;   /* --append-system-prompt text; NULL -> none     */
    const char *tools;           /* --tools value; NULL -> flag omitted (all tools);
                                    "" -> disable every built-in tool; else a list
                                    like "Bash,Read". Empty string is meaningful,
                                    so NULL (not "") is the "unset" sentinel.       */
    int use_subscription;        /* nonzero: unset ANTHROPIC_API_KEY in the child
                                    so the CLI uses your claude.ai login           */
    int allow_customizations;    /* nonzero: drop --safe-mode, so the CLI loads the
                                    user's skills, CLAUDE.md, plugins, hooks, MCP
                                    servers, custom commands and agents. Zero (the
                                    {0} default) keeps the sandboxed behaviour.    */
} claude_opts;

/* Start a persistent headless claude process. Returns NULL on failure. */
claude_client *claude_start(const claude_opts *opts);

/*
 * Send one user turn (plain text) and return the final assistant text (malloc'd,
 * caller frees), or NULL on failure / process death. Blocks until the turn's
 * `result` event. Context is retained across calls by the CLI itself.
 */
char *claude_send(claude_client *c, const char *user_text);

/* Accounting from a turn's `result` event. Token counts are that turn's, not
 * cumulative; context_window is the active model's limit as the CLI reports it
 * (0 when it did not). */
typedef struct {
    long   duration_ms;
    int    num_turns;
    double cost_usd;      /* cumulative for the session, as the CLI reports it */
    long   input_tokens;
    long   output_tokens;
    long   cache_read_tokens;
    long   cache_creation_tokens;
    long   context_tokens; /* latest primary-model request, including output */
    long   context_window;
    int    is_error;
    int    interrupted;   /* the turn ended because claude_interrupt() was sent */
    char   subtype[32];   /* "success", "error_during_execution", ...           */
} claude_result;

/* As claude_send, but also fills *meta (zeroed first) from the result event.
 * `meta` may be NULL. */
char *claude_send_ex(claude_client *c, const char *user_text, claude_result *meta);

/* The model the CLI resolved at startup (from its `system`/`init` event), or
 * NULL until the first turn has been sent. */
const char *claude_model(claude_client *c);

/* How the CLI authenticated, as it reports it: "none" means the claude.ai
 * login (what use_subscription asks for), otherwise the env var it used, e.g.
 * "ANTHROPIC_API_KEY". NULL until the first turn has been sent. */
const char *claude_auth_source(claude_client *c);

/* Clear conversation context by sending Claude Code's /clear command over the
 * existing JSONL stream. Returns nonzero on success; the process stays alive. */
int claude_reset(claude_client *c);

/* The CLI's session id (for resume across restarts), or NULL until known. */
const char *claude_session_id(claude_client *c);

/* When on, claude_send echoes a readable trace of the stream to stderr:
 * assistant text, tool calls, and tool results as they arrive. */
void claude_set_verbose(claude_client *c, int on);

/* One interesting event from the stream, handed to the event callback. */
typedef enum {
    CLAUDE_EV_ASSISTANT,   /* text: an assistant text block                    */
    CLAUDE_EV_THINKING,    /* text: an extended-thinking block                 */
    CLAUDE_EV_TOOL,        /* name + input_json: a tool the model invoked      */
    CLAUDE_EV_TOOL_RESULT, /* text: the tool's output (truncated by the CLI)   */
    CLAUDE_EV_INIT,        /* name: the model the CLI resolved                 */
} claude_event_kind;

typedef struct {
    claude_event_kind kind;
    const char *text;       /* NULL unless the kind documents it */
    const char *name;       /* tool name, or model name for INIT */
    const char *input_json; /* tool input as compact JSON, for TOOL */
} claude_event;

/* Register a callback invoked for each interesting stream event during a turn.
 * Runs on the claude_send thread as events arrive; the event and its strings are
 * borrowed for the duration of the call. Pass NULL to clear. Independent of
 * verbose (this is the programmatic sink; verbose is the stderr one). */
void claude_set_event_cb(claude_client *c,
                         void (*cb)(void *ud, const claude_event *ev),
                         void *ud);

/* Ask the CLI to abandon the in-flight turn (the Agent SDK's interrupt control
 * request). The turn still ends with a result event, so the stream stays usable
 * for the next send. Returns nonzero on success. */
int claude_interrupt(claude_client *c);

/* Register an abort predicate, polled while a turn is in flight. When it first
 * returns nonzero the client interrupts the turn and keeps reading until the
 * CLI's result event, leaving the process ready for the next send. The predicate
 * doubles as a UI tick: it is called roughly every 80ms. */
void claude_set_abort_check(claude_client *c, int (*cb)(void));

/* The tail of anything the CLI wrote to stderr, or NULL if it has been quiet.
 * The child's stderr is captured rather than passed through, so a warning cannot
 * corrupt a caller that is painting its own terminal display. */
const char *claude_last_error(claude_client *c);

/* Terminate and reap the process, free the client. */
void claude_stop(claude_client *c);

#endif /* CLAUDE_H */

/* ======================================================================== */
/*   IMPLEMENTATION                                                          */
/* ======================================================================== */
#ifdef CLAUDE_IMPLEMENTATION

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

#define CLAUDE_ERR_MAX 4096

struct claude_client {
    pid_t pid;
    int   in_fd;              /* write user turns here (child stdin)   */
    int   out_fd;             /* read JSONL events here (child stdout) */
    int   err_fd;             /* read the child's stderr here          */
    char  err[CLAUDE_ERR_MAX];/* tail of that stderr, NUL-terminated   */
    size_t err_len;
    int  (*abort)(void);
    int   verbose;
    void (*on_event)(void *ud, const claude_event *ev);
    void *on_event_ud;
    char  session_id[128];
    char  model[128];
    char  auth[64];
    claude_result *meta;      /* filled from the in-flight turn's result event */
    char *buf;                /* line-assembly buffer for out_fd       */
    size_t len, cap;
};

void claude_set_verbose(claude_client *c, int on) { if (c) c->verbose = on; }

void claude_set_event_cb(claude_client *c,
                         void (*cb)(void *ud, const claude_event *ev),
                         void *ud) {
    if (c) { c->on_event = cb; c->on_event_ud = ud; }
}

const char *claude_model(claude_client *c) {
    return (c && c->model[0]) ? c->model : NULL;
}

/* Read whatever the child has written to stderr, keeping only the tail. Never
 * blocks: the fd is non-blocking. */
static void cl_drain_stderr(claude_client *c) {
    if (c->err_fd < 0) return;
    char tmp[1024];
    for (;;) {
        ssize_t r = read(c->err_fd, tmp, sizeof tmp);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return;
        }
        size_t n = (size_t)r;
        if (n >= CLAUDE_ERR_MAX) {          /* keep only the last chunk */
            memcpy(c->err, tmp + n - (CLAUDE_ERR_MAX - 1), CLAUDE_ERR_MAX - 1);
            c->err_len = CLAUDE_ERR_MAX - 1;
        } else {
            if (c->err_len + n > CLAUDE_ERR_MAX - 1) {
                size_t drop = c->err_len + n - (CLAUDE_ERR_MAX - 1);
                memmove(c->err, c->err + drop, c->err_len - drop);
                c->err_len -= drop;
            }
            memcpy(c->err + c->err_len, tmp, n);
            c->err_len += n;
        }
        c->err[c->err_len] = '\0';
    }
}

const char *claude_last_error(claude_client *c) {
    if (!c) return NULL;
    cl_drain_stderr(c);
    /* Trim trailing newlines so callers can print it as one message. */
    while (c->err_len > 0 && (c->err[c->err_len - 1] == '\n' || c->err[c->err_len - 1] == '\r'))
        c->err[--c->err_len] = '\0';
    return c->err_len ? c->err : NULL;
}

const char *claude_auth_source(claude_client *c) {
    return (c && c->auth[0]) ? c->auth : NULL;
}

claude_client *claude_start(const claude_opts *opts) {
    claude_opts o = opts ? *opts : (claude_opts){0};
    const char *cli = (o.cli_path && *o.cli_path) ? o.cli_path : "claude";

    /* If the child dies, the next write() to its stdin would raise SIGPIPE and
     * (by default) kill the host process instead of returning EPIPE. Ignore it
     * process-wide so claude_send fails cleanly. Note: this is a global setting. */
    signal(SIGPIPE, SIG_IGN);

    int in_pipe[2], out_pipe[2], err_pipe[2];
    if (pipe(in_pipe) != 0) return NULL;
    if (pipe(out_pipe) != 0) { close(in_pipe[0]); close(in_pipe[1]); return NULL; }
    if (pipe(err_pipe) != 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return NULL;
    }
    if (pid == 0) {
        /* child */
        if (dup2(in_pipe[0], STDIN_FILENO) < 0)   _exit(126);
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0) _exit(126);
        if (dup2(err_pipe[1], STDERR_FILENO) < 0) _exit(126);
        /* Close the originals, but never the fd we just installed as stdin/stdout
         * (guards the case where pipe() handed back fd 0/1 because they were
         * already closed before the call). */
        if (in_pipe[0]  != STDIN_FILENO)  close(in_pipe[0]);
        if (in_pipe[1]  != STDIN_FILENO)  close(in_pipe[1]);
        if (out_pipe[0] != STDOUT_FILENO) close(out_pipe[0]);
        if (out_pipe[1] != STDOUT_FILENO) close(out_pipe[1]);
        if (err_pipe[0] != STDERR_FILENO) close(err_pipe[0]);
        if (err_pipe[1] != STDERR_FILENO) close(err_pipe[1]);
        if (o.cwd && *o.cwd) { if (chdir(o.cwd) != 0) _exit(126); }
        if (o.use_subscription) unsetenv("ANTHROPIC_API_KEY");

        /* Build argv: headless, streaming both directions. */
        const char *argv[32];
        int n = 0;
        argv[n++] = cli;
        argv[n++] = "--print";
        argv[n++] = "--input-format";  argv[n++] = "stream-json";
        argv[n++] = "--output-format"; argv[n++] = "stream-json";
        argv[n++] = "--verbose";
        if (!o.allow_customizations) argv[n++] = "--safe-mode";
        if (o.model && *o.model)           { argv[n++] = "--model";           argv[n++] = o.model; }
        if (o.permission_mode && *o.permission_mode) { argv[n++] = "--permission-mode"; argv[n++] = o.permission_mode; }
        if (o.resume_session && *o.resume_session)   { argv[n++] = "--resume"; argv[n++] = o.resume_session; }
        if (o.append_system && *o.append_system)     { argv[n++] = "--append-system-prompt"; argv[n++] = o.append_system; }
        if (o.tools)                                 { argv[n++] = "--tools";                argv[n++] = o.tools; }
        argv[n] = NULL;
        execvp(cli, (char *const *)argv);
        _exit(127);   /* exec failed */
    }

    /* parent */
    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    /* Don't leak our pipe ends into unrelated child execs the host may do (e.g.
     * a sibling fork/exec): an inherited stdin write-end would also defeat the
     * EOF-based grace shutdown in claude_stop. */
    fcntl(in_pipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(err_pipe[0], F_SETFD, FD_CLOEXEC);
    claude_client *c = calloc(1, sizeof *c);
    if (!c) {
        close(in_pipe[1]); close(out_pipe[0]); close(err_pipe[0]);
        kill(pid, SIGKILL); waitpid(pid, NULL, 0);
        return NULL;
    }
    c->pid = pid;
    c->in_fd = in_pipe[1];
    c->out_fd = out_pipe[0];
    c->err_fd = err_pipe[0];
    /* Never block the parent on the child's diagnostics. */
    fcntl(c->err_fd, F_SETFL, O_NONBLOCK);
    return c;
}

void claude_set_abort_check(claude_client *c, int (*cb)(void)) {
    if (c) c->abort = cb;
}

const char *claude_session_id(claude_client *c) {
    return (c && c->session_id[0]) ? c->session_id : NULL;
}

/* Capture session_id from any event that carries one. */
static void cl_note_session(claude_client *c, cJSON *ev) {
    cJSON *sid = cJSON_GetObjectItemCaseSensitive(ev, "session_id");
    if (sid && cJSON_IsString(sid) && sid->valuestring[0])
        snprintf(c->session_id, sizeof c->session_id, "%s", sid->valuestring);
}

static const char *cl_kind_label(claude_event_kind k) {
    switch (k) {
    case CLAUDE_EV_ASSISTANT:   return "assistant";
    case CLAUDE_EV_THINKING:    return "thinking";
    case CLAUDE_EV_TOOL:        return "tool";
    case CLAUDE_EV_TOOL_RESULT: return "tool-result";
    case CLAUDE_EV_INIT:        return "init";
    }
    return "?";
}

/* Emit one interesting stream event to the verbose stderr trace and/or the
 * registered callback. */
static void cl_sink(claude_client *c, const claude_event *ev) {
    const char *shown = ev->text ? ev->text : (ev->name ? ev->name : NULL);
    if (!shown) return;
    if (c->verbose)
        fprintf(stderr, "  [%s] %.400s%s\n", cl_kind_label(ev->kind), shown,
                strlen(shown) > 400 ? " ..." : "");
    if (c->on_event) c->on_event(c->on_event_ud, ev);
}

/* The tool_result content block is either a plain string or an array of typed
 * blocks; return the first text found, or NULL. */
static const char *cl_tool_result_text(cJSON *blk) {
    cJSON *content = cJSON_GetObjectItem(blk, "content");
    if (!content) return NULL;
    if (cJSON_IsString(content)) return content->valuestring;
    if (cJSON_IsArray(content)) {
        cJSON *part;
        cJSON_ArrayForEach(part, content) {
            const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(part, "text"));
            if (t) return t;
        }
    }
    return NULL;
}

static void cl_emit(claude_client *c, cJSON *ev, const char *type) {
    cJSON *message = cJSON_GetObjectItemCaseSensitive(ev, "message");
    cJSON *content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
    if (!cJSON_IsArray(content)) return;
    cJSON *blk;
    cJSON_ArrayForEach(blk, content) {
        const char *bt = cJSON_GetStringValue(cJSON_GetObjectItem(blk, "type"));
        if (!bt) continue;
        claude_event out = {0};
        if (strcmp(type, "assistant") == 0 && strcmp(bt, "text") == 0) {
            out.kind = CLAUDE_EV_ASSISTANT;
            out.text = cJSON_GetStringValue(cJSON_GetObjectItem(blk, "text"));
            cl_sink(c, &out);
        } else if (strcmp(type, "assistant") == 0 && strcmp(bt, "thinking") == 0) {
            out.kind = CLAUDE_EV_THINKING;
            out.text = cJSON_GetStringValue(cJSON_GetObjectItem(blk, "thinking"));
            cl_sink(c, &out);
        } else if (strcmp(type, "assistant") == 0 && strcmp(bt, "tool_use") == 0) {
            cJSON *inp = cJSON_GetObjectItem(blk, "input");
            char *is = inp ? cJSON_PrintUnformatted(inp) : NULL;
            out.kind = CLAUDE_EV_TOOL;
            out.name = cJSON_GetStringValue(cJSON_GetObjectItem(blk, "name"));
            if (!out.name) out.name = "?";
            out.input_json = is;
            cl_sink(c, &out);
            free(is);
        } else if (strcmp(type, "user") == 0 && strcmp(bt, "tool_result") == 0) {
            const char *t = cl_tool_result_text(blk);
            out.kind = CLAUDE_EV_TOOL_RESULT;
            out.text = t ? t : "(result)";
            cl_sink(c, &out);
        }
    }
}

static long cl_long(cJSON *obj, const char *key) {
    cJSON *v = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
    return (v && cJSON_IsNumber(v)) ? (long)v->valuedouble : 0;
}

/* The result event's usage block is aggregate traffic across every model call
 * made during an agentic turn.  It is useful for billing, but it is not the
 * amount occupying the context window: a long tool loop can read the same
 * cached prompt many times and report several windows' worth of aggregate
 * input.  Assistant events carry per-request usage, so retain the latest one
 * from the primary model instead. */
static void cl_note_context(claude_client *c, cJSON *ev, const char *type) {
    if (!c->meta || strcmp(type, "assistant") != 0) return;
    cJSON *message = cJSON_GetObjectItemCaseSensitive(ev, "message");
    cJSON *usage = message ? cJSON_GetObjectItemCaseSensitive(message, "usage") : NULL;
    if (!cJSON_IsObject(usage)) return;

    const char *model = cJSON_GetStringValue(cJSON_GetObjectItem(message, "model"));
    if (c->model[0] && model && strcmp(model, c->model) != 0) return;

    long used = cl_long(usage, "input_tokens") + cl_long(usage, "output_tokens") +
                cl_long(usage, "cache_read_input_tokens") +
                cl_long(usage, "cache_creation_input_tokens");
    if (used > 0) c->meta->context_tokens = used;
}

/* modelUsage is keyed by the concrete model id and may carry several entries
 * when a turn used a helper model; take the window from the entry matching the
 * session model, else the largest one seen. */
static long cl_context_window(claude_client *c, cJSON *ev) {
    cJSON *mu = cJSON_GetObjectItemCaseSensitive(ev, "modelUsage");
    if (!cJSON_IsObject(mu)) return 0;
    long best = 0;
    cJSON *entry;
    cJSON_ArrayForEach(entry, mu) {
        long window = cl_long(entry, "contextWindow");
        if (c->model[0] && entry->string && strcmp(entry->string, c->model) == 0)
            return window;
        if (window > best) best = window;
    }
    return best;
}

static void cl_fill_result(claude_client *c, cJSON *ev) {
    claude_result *m = c->meta;
    if (!m) return;
    cJSON *usage = cJSON_GetObjectItemCaseSensitive(ev, "usage");
    m->duration_ms = cl_long(ev, "duration_ms");
    m->num_turns = (int)cl_long(ev, "num_turns");
    cJSON *cost = cJSON_GetObjectItemCaseSensitive(ev, "total_cost_usd");
    m->cost_usd = (cost && cJSON_IsNumber(cost)) ? cost->valuedouble : 0.0;
    m->input_tokens = cl_long(usage, "input_tokens");
    m->output_tokens = cl_long(usage, "output_tokens");
    m->cache_read_tokens = cl_long(usage, "cache_read_input_tokens");
    m->cache_creation_tokens = cl_long(usage, "cache_creation_input_tokens");
    m->context_window = cl_context_window(c, ev);
    m->is_error = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ev, "is_error"));
    const char *sub = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "subtype"));
    if (sub) snprintf(m->subtype, sizeof m->subtype, "%s", sub);
}

/* Parse one JSONL event line. If it is the turn's `result`, return its text
 * (malloc'd) via *out and return 1. Otherwise return 0. */
static int cl_handle_line(claude_client *c, const char *line, char **out) {
    cJSON *ev = cJSON_Parse(line);
    if (!ev) return 0;                     /* non-JSON noise (shouldn't hit stdout) */
    cl_note_session(c, ev);
    cJSON *type = cJSON_GetObjectItemCaseSensitive(ev, "type");
    const char *ts = (type && cJSON_IsString(type)) ? type->valuestring : "";
    if (strcmp(ts, "system") == 0) {
        const char *auth = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "apiKeySource"));
        if (auth && *auth)
            snprintf(c->auth, sizeof c->auth, "%s", auth);
        const char *model = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "model"));
        if (model && *model) {
            snprintf(c->model, sizeof c->model, "%s", model);
            claude_event out = {.kind = CLAUDE_EV_INIT, .name = c->model};
            cl_sink(c, &out);
        }
    }
    cl_note_context(c, ev, ts);
    if ((c->verbose || c->on_event) && *ts) cl_emit(c, ev, ts);
    int is_result = strcmp(ts, "result") == 0;
    if (is_result) {
        cl_fill_result(c, ev);
        cJSON *res = cJSON_GetObjectItemCaseSensitive(ev, "result");
        const char *text = (res && cJSON_IsString(res)) ? res->valuestring : "";
        *out = strdup(text ? text : "");
    }
    cJSON_Delete(ev);
    return is_result;
}

/* Serialize `msg` (consumed) as one JSONL line on the child's stdin. */
static int cl_write_json(claude_client *c, cJSON *msg) {
    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!json) return 0;

    size_t jl = strlen(json);
    char *withnl = realloc(json, jl + 2);
    if (!withnl) { free(json); return 0; }
    json = withnl; json[jl] = '\n'; json[jl + 1] = '\0';

    for (size_t off = 0; off < jl + 1; ) {
        ssize_t w = write(c->in_fd, json + off, (jl + 1) - off);
        if (w < 0) { if (errno == EINTR) continue; free(json); return 0; }
        off += (size_t)w;
    }
    free(json);
    return 1;
}

int claude_interrupt(claude_client *c) {
    if (!c) return 0;
    cJSON *msg = cJSON_CreateObject();
    if (!msg) return 0;
    cJSON_AddStringToObject(msg, "type", "control_request");
    cJSON_AddStringToObject(msg, "request_id", "claude_h_interrupt");
    cJSON *req = cJSON_AddObjectToObject(msg, "request");
    cJSON_AddStringToObject(req, "subtype", "interrupt");
    return cl_write_json(c, msg);
}

static char *cl_send(claude_client *c, const char *user_text, int content_block) {
    if (!c || !user_text) return NULL;

    /* Frame the user turn as one JSONL line (cJSON handles all escaping). */
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "user");
    cJSON *inner = cJSON_AddObjectToObject(msg, "message");
    cJSON_AddStringToObject(inner, "role", "user");
    if (content_block) {
        cJSON *content = cJSON_AddArrayToObject(inner, "content");
        cJSON *text = cJSON_CreateObject();
        cJSON_AddStringToObject(text, "type", "text");
        cJSON_AddStringToObject(text, "text", user_text);
        cJSON_AddItemToArray(content, text);
    } else {
        cJSON_AddStringToObject(inner, "content", user_text);
    }
    if (!cl_write_json(c, msg)) return NULL;

    /* Read events until this turn's result. */
    char *result = NULL;
    char tmp[8192];
    int interrupted = 0;
    for (;;) {
        /* Abort mid-turn: ask the CLI to abandon the turn, then keep reading to
         * its result event so the stream stays aligned for the next send. */
        if (c->abort && !interrupted && c->abort()) {
            interrupted = 1;
            claude_interrupt(c);
            if (c->meta) c->meta->interrupted = 1;
        }
        /* Short enough that the abort predicate doubles as a UI tick. */
        struct pollfd pfds[2] = { { c->out_fd, POLLIN, 0 }, { c->err_fd, POLLIN, 0 } };
        int pr = poll(pfds, 2, 80);
        if (pr < 0) { if (errno == EINTR) continue; c->len = 0; break; }
        if (pfds[1].revents) cl_drain_stderr(c);     /* keep the pipe from filling */
        if (!(pfds[0].revents & (POLLIN | POLLHUP))) continue;
        ssize_t r = read(c->out_fd, tmp, sizeof tmp);
        if (r <= 0) { c->len = 0; break; }           /* EOF / error: process gone */

        /* append to line buffer */
        if (c->len + (size_t)r + 1 > c->cap) {
            size_t nc = (c->len + (size_t)r + 1) * 2;
            char *nb = realloc(c->buf, nc);
            if (!nb) { c->len = 0; break; }
            c->buf = nb; c->cap = nc;
        }
        memcpy(c->buf + c->len, tmp, (size_t)r);
        c->len += (size_t)r;
        c->buf[c->len] = '\0';

        /* process complete lines */
        char *start = c->buf, *nl;
        while ((nl = memchr(start, '\n', c->len - (size_t)(start - c->buf)))) {
            *nl = '\0';
            if (cl_handle_line(c, start, &result)) {
                /* keep any bytes after this line for next time */
                size_t consumed = (size_t)(nl + 1 - c->buf);
                size_t rest = c->len - consumed;
                memmove(c->buf, c->buf + consumed, rest);
                c->len = rest;
                return result;
            }
            start = nl + 1;
        }
        /* shift the unfinished tail to the front */
        size_t consumed = (size_t)(start - c->buf);
        if (consumed) { memmove(c->buf, start, c->len - consumed); c->len -= consumed; }
    }
    return result;   /* NULL unless a result slipped in just before EOF */
}

char *claude_send(claude_client *c, const char *user_text) {
    return cl_send(c, user_text, 0);
}

char *claude_send_ex(claude_client *c, const char *user_text, claude_result *meta) {
    if (!c) return NULL;
    if (meta) memset(meta, 0, sizeof *meta);
    c->meta = meta;
    char *r = cl_send(c, user_text, 0);
    c->meta = NULL;
    return r;
}

int claude_reset(claude_client *c) {
    char *result = cl_send(c, "/clear", 1);
    if (!result) return 0;
    free(result);
    return 1;
}

/* Poll for the child's exit for up to `ms`. Returns nonzero once reaped. */
static int cl_reap_within(claude_client *c, int ms) {
    for (int waited = 0; waited < ms; waited += 5) {
        if (waitpid(c->pid, NULL, WNOHANG) == c->pid) { c->pid = 0; return 1; }
        usleep(5000);
    }
    return 0;
}

void claude_stop(claude_client *c) {
    if (!c) return;
    if (c->in_fd >= 0) close(c->in_fd);     /* EOF on stdin asks it to exit */
    if (c->pid > 0) {
        /* We only stop between turns, when the CLI has already flushed its
         * transcript, so there is nothing to wait for. The Node runtime takes
         * a few hundred ms to unwind on its own, which a caller quitting an
         * interactive session would feel, so escalate immediately. */
        kill(c->pid, SIGTERM);
        if (!cl_reap_within(c, 50)) {
            kill(c->pid, SIGKILL);
            waitpid(c->pid, NULL, 0);
            c->pid = 0;
        }
    }
    if (c->out_fd >= 0) close(c->out_fd);
    if (c->err_fd >= 0) close(c->err_fd);
    free(c->buf);
    free(c);
}

#endif /* CLAUDE_IMPLEMENTATION */
