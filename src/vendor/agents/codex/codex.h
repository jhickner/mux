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
    int ephemeral;              /* nonzero: do not materialize the thread on disk */
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

typedef struct {
    int  available;
    int  used_percent;
    long resets_at;
    long window_minutes;
} codex_rate_limit;

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
    CODEX_EV_CWD,           /* text: the directory the thread works in now */
    CODEX_EV_TRUST,         /* text: project path awaiting trust approval   */
    CODEX_EV_WARNING,       /* text: actionable app-server config warning  */
} codex_event_kind;

typedef struct {
    codex_event_kind kind;
    const char *text;       /* assistant/thinking text, or tool output */
    const char *name;       /* tool name, for CODEX_EV_TOOL           */
    const char *input_json; /* compact tool input, for CODEX_EV_TOOL  */
    const char *diff;       /* unified patch, for a file-change result */
    int failed;             /* TOOL_RESULT: status was not completed   */
} codex_event;

void codex_set_event_cb(codex_client *c,
                        void (*cb)(void *ud, const codex_event *ev),
                        void *ud);
void codex_set_abort_check(codex_client *c, int (*cb)(void));
/* A config warning can arrive while app-server is warming in the background.
 * Watch this fd between turns and pump it on the UI thread. */
int codex_idle_fd(codex_client *c);
int codex_idle_pump(codex_client *c);
/* Persist this path as trusted in the user's Codex config. */
int codex_trust_project(codex_client *c, const char *path);
/* Override subsequent turns' reasoning effort. NULL clears the override. */
int codex_set_effort(codex_client *c, const char *effort);
/* Override if one was set, else the config/stream default, or NULL. */
const char *codex_effort(codex_client *c);
/* The model id the app-server resolved, else the requested one, or NULL. */
const char *codex_model(codex_client *c);
/* Latest context occupancy reported by thread/tokenUsage/updated. */
void codex_usage(codex_client *c, long *context_tokens, long *context_window);
/* Latest primary subscription window from the account rate-limit methods. */
void codex_get_rate_limit(codex_client *c, codex_rate_limit *out);
/* Tail of app-server stderr, without config warnings surfaced as events. */
const char *codex_last_error(codex_client *c);
void codex_stop(codex_client *c);

#endif /* CODEX_H */

#ifdef CODEX_IMPLEMENTATION

#include <errno.h>
#include <fcntl.h>
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
#define CX_ERR_MAX 4096
#define CX_WARNING_MAX 1024

struct codex_client {
    pid_t pid;
    int in_fd, out_fd, err_fd, notice_fd[2], next_id, verbose;
    int (*abort)(void);
    void (*on_event)(void *ud, const codex_event *ev);
    void *on_event_ud;
    char *model, *effort, *sandbox, *sys, *resume, *project;
    int effort_changed, ephemeral;
    char session_id[128];
    char resolved[32];         /* config or stream effort when none was set */
    char resolved_model[64];   /* the model id the app-server picked          */
    char cwd[4096];            /* the working directory it reported last      */
    long context_tokens, context_window;
    codex_rate_limit rate_limit;
    char *buf;
    size_t len, cap;
    char err[CX_ERR_MAX];
    size_t err_len;
    char warning[CX_WARNING_MAX];
    codex_event_kind warning_kind;
    int warning_ready;
    pthread_mutex_t warning_mu;
    int warning_mu_ready;
    pthread_t warm_thread;
    int warm_joinable;
    atomic_int warm_state;     /* 0 while starting, 1 ready, -1 failed */
};

static char *cx_dup(const char *s) { return (s && *s) ? strdup(s) : NULL; }

/* Top-level model_reasoning_effort only: table keys belong to a profile. */
static void cx_read_effort_file(codex_client *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    int in_table = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0' || *p == '\n') continue;
        if (*p == '[') { in_table = 1; continue; }
        if (in_table) continue;
        if (strncmp(p, "model_reasoning_effort", 22) != 0) continue;
        p += 22;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '"' && *p != '\'') continue;
        char q = *p++;
        char *end = strchr(p, q);
        if (!end || end - p <= 0 || end - p >= (long)sizeof c->resolved)
            continue;
        memcpy(c->resolved, p, (size_t)(end - p));
        c->resolved[end - p] = '\0';
        break;
    }
    fclose(f);
}

static void cx_seed_effort(codex_client *c, const char *cwd) {
    if (c->effort && *c->effort) {
        snprintf(c->resolved, sizeof c->resolved, "%s", c->effort);
        return;
    }
    const char *home = getenv("HOME");
    char path[512];
    if (home && *home) {
        snprintf(path, sizeof path, "%s/.codex/config.toml", home);
        cx_read_effort_file(c, path);
    }
    if (cwd && *cwd) {
        snprintf(path, sizeof path, "%s/.codex/config.toml", cwd);
        cx_read_effort_file(c, path);
    }
}

static void cx_note_effort(codex_client *c, cJSON *obj) {
    if (!cJSON_IsObject(obj)) return;
    const char *e = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(obj, "effort"));
    /* thread/start and thread/resume answer with the camelCase spelling. */
    if (!e || !*e)
        e = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(obj, "reasoningEffort"));
    if (!e || !*e) {
        cJSON *mode = cJSON_GetObjectItemCaseSensitive(obj, "collaboration_mode");
        cJSON *settings = cJSON_IsObject(mode) ?
            cJSON_GetObjectItemCaseSensitive(mode, "settings") : NULL;
        e = settings ? cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(settings, "reasoning_effort")) : NULL;
    }
    if (e && *e)
        snprintf(c->resolved, sizeof c->resolved, "%s", e);
}

/* The caller's model option is a request; the app-server answers with the id it
 * actually resolved, which is what a caller naming the model should show. */
static void cx_note_model(codex_client *c, cJSON *obj) {
    if (!cJSON_IsObject(obj)) return;
    const char *m = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(obj, "model"));
    if (m && *m)
        snprintf(c->resolved_model, sizeof c->resolved_model, "%s", m);
}

static void cx_emit(codex_client *c, const codex_event *ev) {
    static const char *const kinds[] = {
        "assistant", "thinking", "tool", "tool-result", "cwd", "trust", "warning"
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

/* App-server mirrors configWarning notifications to stderr with timestamps and
 * Rust log metadata. Capture that stream so the structured notification can be
 * rendered by the host instead. Keep the tail for actual startup failures. */
static void cx_drain_stderr(codex_client *c) {
    if (c->err_fd < 0) return;
    char tmp[1024];
    for (;;) {
        ssize_t r = read(c->err_fd, tmp, sizeof tmp);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return;
        }
        size_t n = (size_t)r;
        if (n >= CX_ERR_MAX) {
            memcpy(c->err, tmp + n - (CX_ERR_MAX - 1), CX_ERR_MAX - 1);
            c->err_len = CX_ERR_MAX - 1;
        } else {
            if (c->err_len + n > CX_ERR_MAX - 1) {
                size_t drop = c->err_len + n - (CX_ERR_MAX - 1);
                memmove(c->err, c->err + drop, c->err_len - drop);
                c->err_len -= drop;
            }
            memcpy(c->err + c->err_len, tmp, n);
            c->err_len += n;
        }
        c->err[c->err_len] = '\0';
    }
}

/* Remove only the stderr copy of the project-trust warning. Other diagnostics
 * in the same tail remain available through codex_last_error(). */
static void cx_strip_trust_warning(codex_client *c) {
    char *start = strstr(c->err,
        "Project-local config, hooks, and exec policies are disabled");
    if (!start) return;
    char *block = start;
    while (block > c->err && block[-1] != '\n') block--;
    char *end = strstr(start, "config.toml.");
    if (!end) return;
    end += strlen("config.toml.");
    while (*end == '\r' || *end == '\n') end++;
    size_t used = (size_t)(end - block);
    memmove(block, end, strlen(end) + 1);
    c->err_len -= used;
}

static void cx_queue_warning(codex_client *c, codex_event_kind kind, const char *text) {
    if (!text || !*text || !c->warning_mu_ready) return;
    int signal = 0;
    pthread_mutex_lock(&c->warning_mu);
    if (!c->warning_ready) {
        snprintf(c->warning, sizeof c->warning, "%s", text);
        c->warning_kind = kind;
        c->warning_ready = 1;
        signal = 1;
    }
    pthread_mutex_unlock(&c->warning_mu);
    if (signal && c->notice_fd[1] >= 0) {
        char byte = 1;
        ssize_t ignored = write(c->notice_fd[1], &byte, 1);
        (void)ignored;
    }
}

/* A turn may override the thread's cwd, so the app-server's copy is the one
 * that says where tools actually run. */
static void cx_note_cwd(codex_client *c, cJSON *obj) {
    if (!cJSON_IsObject(obj)) return;
    const char *d = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, "cwd"));
    if (!d || !*d || !strcmp(d, c->cwd)) return;
    snprintf(c->cwd, sizeof c->cwd, "%s", d);
    codex_event ev = { .kind = CODEX_EV_CWD, .text = c->cwd };
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
        struct pollfd p[2] = {
            { c->out_fd, POLLIN, 0 }, { c->err_fd, POLLIN, 0 }
        };
        int pr = poll(p, 2, CX_TICK_MS);
        if (pr < 0) { if (errno == EINTR) continue; return -1; }
        if (!pr) continue;
        if (p[1].revents) {
            cx_drain_stderr(c);
            if (p[1].revents & (POLLHUP | POLLERR)) {
                close(c->err_fd);
                c->err_fd = -1;
            }
        }
        if (!(p[0].revents & (POLLIN | POLLHUP))) continue;
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

static void cx_note_usage(codex_client *c, cJSON *params);

/* Both the snapshot response and rolling notification carry a rateLimits
 * object. Rolling updates are sparse, so absent/null values preserve the last
 * good value rather than clearing it. */
static void cx_note_rate_limit(codex_client *c, cJSON *container) {
    cJSON *limits = container ? cJSON_GetObjectItemCaseSensitive(container, "rateLimits") : NULL;
    cJSON *primary = cJSON_IsObject(limits) ?
        cJSON_GetObjectItemCaseSensitive(limits, "primary") : NULL;
    if (!cJSON_IsObject(primary)) return;
    cJSON *used = cJSON_GetObjectItemCaseSensitive(primary, "usedPercent");
    cJSON *resets = cJSON_GetObjectItemCaseSensitive(primary, "resetsAt");
    cJSON *window = cJSON_GetObjectItemCaseSensitive(primary, "windowDurationMins");
    if (cJSON_IsNumber(used)) {
        int percent = (int)(used->valuedouble + 0.5);
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        c->rate_limit.used_percent = percent;
        c->rate_limit.available = 1;
    }
    if (cJSON_IsNumber(resets) && resets->valuedouble > 0)
        c->rate_limit.resets_at = (long)resets->valuedouble;
    if (cJSON_IsNumber(window) && window->valuedouble > 0)
        c->rate_limit.window_minutes = (long)window->valuedouble;
}

static int cx_handle_notification(codex_client *c, cJSON *msg) {
    const char *method = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(msg, "method"));
    cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
    if (params) {
        cJSON *turn = cJSON_GetObjectItemCaseSensitive(params, "turn");
        cx_note_effort(c, params);
        cx_note_effort(c, turn);
        cx_note_model(c, params);
        cx_note_model(c, turn);
    }
    if (method && !strcmp(method, "thread/settings/updated")) {
        cx_note_cwd(c, params ? cJSON_GetObjectItemCaseSensitive(params, "threadSettings")
                              : NULL);
        return 1;
    }
    if (method && !strcmp(method, "thread/tokenUsage/updated")) {
        cx_note_usage(c, params);
        return 1;
    }
    if (method && !strcmp(method, "account/rateLimits/updated")) {
        cx_note_rate_limit(c, params);
        return 1;
    }
    if (method && !strcmp(method, "configWarning")) {
        const char *summary = params ? cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(params, "summary")) : NULL;
        if (summary && strstr(summary, "until the project is trusted")) {
            cx_queue_warning(c, CODEX_EV_TRUST,
                             c->project ? c->project : "this folder");
        } else if (summary && *summary) {
            cx_queue_warning(c, CODEX_EV_WARNING, summary);
        }
        /* The same warning was logged to stderr; it is not a process failure. */
        cx_drain_stderr(c);
        cx_strip_trust_warning(c);
        return 1;
    }
    return 0;
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
        else cx_handle_notification(c, msg);
        cJSON_Delete(msg);
    }
}

static int cx_initialize(codex_client *c) {
    cJSON *p = cJSON_CreateObject(), *info = cJSON_CreateObject();
    cJSON *capabilities = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", "codex.h");
    cJSON_AddStringToObject(info, "version", "1");
    cJSON_AddItemToObject(p, "clientInfo", info);
    /* Code Mode tools are Responses API custom-tool items rather than the
     * commandExecution items emitted by the legacy shell. Ask app-server to
     * expose those raw items alongside its normalized lifecycle events. */
    cJSON_AddBoolToObject(capabilities, "experimentalApi", 1);
    cJSON_AddItemToObject(p, "capabilities", capabilities);
    int id = cx_request(c, "initialize", p);
    cJSON *r = id ? cx_wait_response(c, id) : NULL;
    if (!r) return 0;
    cJSON_Delete(r);
    cJSON *note = cJSON_CreateObject();
    cJSON_AddStringToObject(note, "method", "initialized");
    return cx_write(c, note);
}

static void cx_read_rate_limit(codex_client *c) {
    int id = cx_request(c, "account/rateLimits/read", cJSON_CreateNull());
    cJSON *r = id ? cx_wait_response(c, id) : NULL;
    if (!r) return;                 /* older app-server: quota stays unavailable */
    cJSON *result = cJSON_GetObjectItemCaseSensitive(r, "result");
    cx_note_rate_limit(c, result);
    cJSON_Delete(r);
}

static int cx_open_thread(codex_client *c, const char *resume) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "approvalPolicy", "never");
    cJSON_AddStringToObject(p, "sandbox", c->sandbox);
    cJSON_AddBoolToObject(p, "experimentalRawEvents", 1);
    if (resume && *resume) {
        cJSON_AddStringToObject(p, "threadId", resume);
    } else {
        if (c->model) cJSON_AddStringToObject(p, "model", c->model);
        if (c->sys) cJSON_AddStringToObject(p, "developerInstructions", c->sys);
        if (c->ephemeral) cJSON_AddBoolToObject(p, "ephemeral", 1);
    }
    int id = cx_request(c, resume && *resume ? "thread/resume" : "thread/start", p);
    cJSON *r = id ? cx_wait_response(c, id) : NULL;
    if (!r) return 0;
    cJSON *result = cJSON_GetObjectItemCaseSensitive(r, "result");
    cJSON *thread = result ? cJSON_GetObjectItemCaseSensitive(result, "thread") : NULL;
    const char *sid = thread ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(thread, "id")) : NULL;
    if (sid) snprintf(c->session_id, sizeof c->session_id, "%s", sid);
    cx_note_effort(c, thread);
    cx_note_effort(c, result);      /* the settled model/effort sit beside the
                                       thread rather than inside it */
    cx_note_model(c, result);
    cx_note_cwd(c, result);
    cJSON_Delete(r); return sid != NULL;
}

/* The app-server handshake and thread creation are local but noticeably slower
 * than drawing the prompt. Do them while the user is reading or typing instead
 * of holding the whole interface behind them. */
static void *cx_warm(void *arg) {
    codex_client *c = arg;
    int initialized = cx_initialize(c);
    if (initialized) cx_read_rate_limit(c);
    int ok = initialized && cx_open_thread(c, c->resume);
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
    c->in_fd = c->out_fd = c->err_fd = -1;
    c->notice_fd[0] = c->notice_fd[1] = -1;
    c->next_id = 1;
    if (pthread_mutex_init(&c->warning_mu, NULL)) { free(c); return NULL; }
    c->warning_mu_ready = 1;
    c->model = cx_dup(o.model); c->effort = cx_dup(o.effort);
    c->ephemeral = o.ephemeral;
    c->sys = cx_dup(o.append_system);
    cx_seed_effort(c, o.cwd);
    c->resume = cx_dup(o.resume_session);
    c->project = cx_dup(o.cwd);
    c->sandbox = strdup(o.bypass_approvals ? "danger-full-access" :
                        (o.sandbox && *o.sandbox ? o.sandbox : "workspace-write"));
    if (!c->sandbox) { codex_stop(c); return NULL; }
    int in[2], out[2], err[2];
    if (pipe(in)) { codex_stop(c); return NULL; }
    if (pipe(out)) { close(in[0]); close(in[1]); codex_stop(c); return NULL; }
    if (pipe(err)) {
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        codex_stop(c); return NULL;
    }
    signal(SIGPIPE, SIG_IGN);
    pid_t pid = fork();
    if (pid < 0) {
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        close(err[0]); close(err[1]);
        codex_stop(c); return NULL;
    }
    if (!pid) {
        if (dup2(in[0], STDIN_FILENO) < 0 || dup2(out[1], STDOUT_FILENO) < 0 ||
            dup2(err[1], STDERR_FILENO) < 0)
            _exit(126);
        if (in[0] != STDIN_FILENO) close(in[0]);
        if (in[1] != STDIN_FILENO) close(in[1]);
        if (out[0] != STDOUT_FILENO) close(out[0]);
        if (out[1] != STDOUT_FILENO) close(out[1]);
        if (err[0] != STDERR_FILENO) close(err[0]);
        if (err[1] != STDERR_FILENO) close(err[1]);
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
    close(in[0]); close(out[1]); close(err[1]);
    /* Keep the agent pipes out of every child we later fork (shell, git, …),
       matching claude.h/grok.h/pi.h. */
    fcntl(in[1], F_SETFD, FD_CLOEXEC);
    fcntl(out[0], F_SETFD, FD_CLOEXEC);
    fcntl(err[0], F_SETFD, FD_CLOEXEC);
    fcntl(err[0], F_SETFL, O_NONBLOCK);
    c->pid = pid; c->in_fd = in[1]; c->out_fd = out[0]; c->err_fd = err[0];
    if (pipe(c->notice_fd)) { codex_stop(c); return NULL; }
    fcntl(c->notice_fd[0], F_SETFD, FD_CLOEXEC);
    fcntl(c->notice_fd[1], F_SETFD, FD_CLOEXEC);
    fcntl(c->notice_fd[0], F_SETFL, O_NONBLOCK);
    fcntl(c->notice_fd[1], F_SETFL, O_NONBLOCK);
    atomic_init(&c->warm_state, 0);
    if (!pthread_create(&c->warm_thread, NULL, cx_warm, c))
        c->warm_joinable = 1;
    else
        cx_warm(c);             /* rare resource failure: retain old semantics */
    return c;
}

void codex_set_verbose(codex_client *c, int on) { if (c) c->verbose = on; }
void codex_set_abort_check(codex_client *c, int (*cb)(void)) { if (c) c->abort = cb; }

int codex_idle_fd(codex_client *c) {
    return c && c->pid > 0 ? c->notice_fd[0] : -1;
}

int codex_idle_pump(codex_client *c) {
    if (!c || c->notice_fd[0] < 0) return 0;
    char bytes[64];
    while (read(c->notice_fd[0], bytes, sizeof bytes) > 0) {}
    pthread_mutex_lock(&c->warning_mu);
    char *warning = c->warning_ready ? strdup(c->warning) : NULL;
    codex_event_kind kind = c->warning_kind;
    c->warning_ready = 0;
    c->warning[0] = '\0';
    pthread_mutex_unlock(&c->warning_mu);
    if (warning) {
        codex_event ev = { .kind = kind, .text = warning };
        cx_emit(c, &ev);
        free(warning);
    }
    return 0;
}

int codex_trust_project(codex_client *c, const char *path) {
    if (!c || !path || !*path || !cx_await_ready(c)) return 0;
    cJSON *p = cJSON_CreateObject();
    cJSON *projects = cJSON_CreateObject();
    cJSON *project = cJSON_CreateObject();
    if (!p || !projects || !project) {
        cJSON_Delete(p); cJSON_Delete(projects); cJSON_Delete(project);
        return 0;
    }
    cJSON_AddStringToObject(project, "trust_level", "trusted");
    cJSON_AddItemToObject(projects, path, project);
    cJSON_AddStringToObject(p, "keyPath", "projects");
    cJSON_AddItemToObject(p, "value", projects);
    cJSON_AddStringToObject(p, "mergeStrategy", "upsert");
    int id = cx_request(c, "config/value/write", p);
    cJSON *r = id ? cx_wait_response(c, id) : NULL;
    if (!r) return 0;
    cJSON *result = cJSON_GetObjectItemCaseSensitive(r, "result");
    const char *status = result ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(result, "status")) : NULL;
    int ok = status && !strcmp(status, "ok");
    cJSON_Delete(r);
    return ok;
}

const char *codex_last_error(codex_client *c) {
    if (!c) return NULL;
    cx_drain_stderr(c);
    cx_strip_trust_warning(c);
    while (c->err_len && (c->err[c->err_len - 1] == '\n' ||
                          c->err[c->err_len - 1] == '\r'))
        c->err[--c->err_len] = '\0';
    return c->err_len ? c->err : NULL;
}

const char *codex_effort(codex_client *c) {
    if (!c) return NULL;
    if (c->effort && *c->effort) return c->effort;
    return c->resolved[0] ? c->resolved : NULL;
}

const char *codex_model(codex_client *c) {
    if (!c) return NULL;
    if (c->resolved_model[0]) return c->resolved_model;
    return c->model && *c->model ? c->model : NULL;
}

int codex_set_effort(codex_client *c, const char *effort) {
    if (!c) return 0;
    free(c->effort);
    c->effort = cx_dup(effort);
    c->effort_changed = 1;
    c->resolved[0] = '\0';
    if (c->effort && *c->effort)
        snprintf(c->resolved, sizeof c->resolved, "%s", c->effort);
    else
        cx_seed_effort(c, NULL);
    return 1;
}
void codex_usage(codex_client *c, long *tokens, long *window) {
    if (tokens) *tokens = c ? c->context_tokens : 0;
    if (window) *window = c ? c->context_window : 0;
}
void codex_get_rate_limit(codex_client *c, codex_rate_limit *out) {
    if (!out) return;
    if (!c || atomic_load_explicit(&c->warm_state, memory_order_acquire) != 1) {
        *out = (codex_rate_limit){0};
        return;
    }
    *out = c->rate_limit;
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

/* Code Mode wraps ordinary tool calls in a small JavaScript program. Recover
 * the common, unambiguous one-command form so it renders like Codex's native
 * shell row. Anything dynamic or multi-tool stays visible as an Exec program. */
static int cx_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

static int cx_ident_char(char c) {
    return cx_ident_start(c) || (c >= '0' && c <= '9');
}

/* Find a nested tool reference without mistaking command text containing
 * `tools.` for another invocation. */
static const char *cx_tool_ref(const char *text) {
    char quote = 0;
    for (const char *p = text; *p; p++) {
        if (quote) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == quote) quote = 0;
            continue;
        }
        if (*p == '"' || *p == '\'' || *p == '`') {
            quote = *p;
            continue;
        }
        if (!strncmp(p, "tools.", 6)) return p;
    }
    return NULL;
}

/* exec's generated argument is JSON-shaped JavaScript. Codex sometimes leaves
 * identifier keys such as `cmd` unquoted, so quote those keys while copying the
 * one object literal; dynamic expressions deliberately fail cJSON parsing. */
static cJSON *cx_exec_args(const char *start, const char **after) {
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
    if (*start != '{') return NULL;
    size_t cap = strlen(start) * 2 + 1, out = 0;
    char *json = malloc(cap);
    if (!json) return NULL;

    int depth = 0, expect_key = 0;
    char quote = 0;
    const char *p = start;
    for (; *p; p++) {
        char ch = *p;
        if (quote) {
            json[out++] = ch;
            if (ch == '\\' && p[1]) json[out++] = *++p;
            else if (ch == quote) quote = 0;
            continue;
        }
        if (ch == '\'' || ch == '`') break; /* not JSON-compatible; keep fallback */
        if (ch == '"') {
            quote = ch;
            json[out++] = ch;
            expect_key = 0;
            continue;
        }
        if (ch == '{') {
            depth++;
            expect_key = 1;
            json[out++] = ch;
            continue;
        }
        if (ch == '}') {
            json[out++] = ch;
            if (--depth == 0) { p++; break; }
            expect_key = 0;
            continue;
        }
        if (ch == ',') {
            expect_key = 1;
            json[out++] = ch;
            continue;
        }
        if (expect_key && cx_ident_start(ch)) {
            const char *ident_end = p + 1;
            while (cx_ident_char(*ident_end)) ident_end++;
            const char *colon = ident_end;
            while (*colon == ' ' || *colon == '\t' || *colon == '\r' || *colon == '\n')
                colon++;
            if (*colon == ':') {
                json[out++] = '"';
                while (p < ident_end) json[out++] = *p++;
                json[out++] = '"';
                p--;
                expect_key = 0;
                continue;
            }
        }
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') expect_key = 0;
        json[out++] = ch;
    }
    json[out] = '\0';
    cJSON *args = depth == 0 && p > start ? cJSON_Parse(json) : NULL;
    free(json);
    if (!args) return NULL;
    *after = p;
    return args;
}

static char *cx_nested_command_input(const char *raw) {
    static const char marker[] = "tools.exec_command(";
    const char *call = cx_tool_ref(raw);
    if (!call || strncmp(call, marker, sizeof marker - 1) ||
        cx_tool_ref(call + sizeof marker - 1)) return NULL;

    const char *end = NULL;
    cJSON *args = cx_exec_args(call + sizeof marker - 1, &end);
    while (end && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) end++;
    if (!cJSON_IsObject(args) || !end || *end != ')') {
        cJSON_Delete(args);
        return NULL;
    }
    const char *command = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(args, "cmd"));
    const char *cwd = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(args, "workdir"));
    if (!command) {
        cJSON_Delete(args);
        return NULL;
    }

    cJSON *input = cJSON_CreateObject();
    cJSON_AddStringToObject(input, "command", command);
    if (cwd) cJSON_AddStringToObject(input, "cwd", cwd);
    char *json = cJSON_PrintUnformatted(input);
    cJSON_Delete(input);
    cJSON_Delete(args);
    return json;
}

static char *cx_custom_input(cJSON *item, int *nested_command) {
    *nested_command = 0;
    const char *raw = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(item, "input"));
    if (!raw) return NULL;
    char *json = cx_nested_command_input(raw);
    if (json) {
        *nested_command = 1;
        return json;
    }
    cJSON *input = cJSON_CreateObject();
    cJSON_AddStringToObject(input, "command", raw);
    json = cJSON_PrintUnformatted(input);
    cJSON_Delete(input);
    return json;
}

/* FunctionCallOutputBody is either a string or an array of Responses content
 * items. The current exec tool returns input_text items; join those in order
 * and ignore non-text media that this terminal UI cannot preview. */
static char *cx_custom_output(cJSON *output) {
    char *text = NULL;
    if (cJSON_IsString(output)) {
        text = strdup(output->valuestring);
    } else if (cJSON_IsArray(output)) {
        int n = cJSON_GetArraySize(output);
        for (int i = 0; i < n; i++) {
            cJSON *part = cJSON_GetArrayItem(output, i);
            const char *s = part ? cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(part, "text")) : NULL;
            if (s) cx_append(&text, s);
        }
    } else {
        return NULL;
    }
    if (text && !strncmp(text, "Script completed\n", 17)) {
        static const char marker[] = "Output:\n";
        char *body = strstr(text + 17, marker);
        if (body && (body == text + 17 || body[-1] == '\n'))
            memmove(text, body + sizeof marker - 1,
                    strlen(body + sizeof marker - 1) + 1);
    }
    return text;
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
            const char *err = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(item, "error"));
            if (!err) err = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(item, "message"));
            int failed = status && strcmp(status, "completed") != 0;
            const char *text = out ? out : (err ? err : NULL);
            if (!text && failed) text = (status && strcmp(status, "failed")) ? status : "failed";
            if (!text) text = "";
            codex_event ev = {
                .kind = CODEX_EV_TOOL_RESULT,
                .text = text,
                .failed = failed,
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
            const char *err = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(item, "error"));
            if (!err) err = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(item, "message"));
            int completed = status && !strcmp(status, "completed");
            char *diff = completed ? cx_file_diff(changes) : NULL;
            const char *text = "";
            if (!completed)
                text = err ? err : ((status && strcmp(status, "failed")) ? status : "failed");
            codex_event ev = {
                .kind = CODEX_EV_TOOL_RESULT,
                .text = text,
                .diff = diff,
                .failed = !completed,
            };
            cx_emit(c, &ev);
            free(diff);
        }
    }
}

/* Raw response items have only a completed notification. For a custom call it
 * arrives before the tool runs, while its matching output item arrives after,
 * so they still form the start/result pair expected by the renderer. */
static void cx_raw_item_event(codex_client *c, cJSON *params) {
    cJSON *item = params ? cJSON_GetObjectItemCaseSensitive(params, "item") : NULL;
    const char *type = item ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(item, "type")) : NULL;
    if (!type) return;
    if (!strcmp(type, "custom_tool_call")) {
        const char *name = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(item, "name"));
        int nested_command;
        char *input = cx_custom_input(item, &nested_command);
        codex_event ev = {
            .kind = CODEX_EV_TOOL,
            .name = nested_command ? "Shell" : (name ? name : "Tool"),
            .input_json = input,
        };
        cx_emit(c, &ev);
        free(input);
    } else if (!strcmp(type, "custom_tool_call_output")) {
        char *text = cx_custom_output(
            cJSON_GetObjectItemCaseSensitive(item, "output"));
        codex_event ev = {
            .kind = CODEX_EV_TOOL_RESULT,
            .text = text ? text : "",
        };
        cx_emit(c, &ev);
        free(text);
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
    cx_note_effort(c, turn);
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
        } else if (method && cx_handle_notification(c, msg)) {
            /* accounting notification retained above */
        } else if (method && !strcmp(method, "item/agentMessage/delta")) {
            const char *d = params ? cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(params, "delta")) : NULL;
            if (d) { cx_append(&answer, d); cx_text_event(c, CODEX_EV_ASSISTANT, d); }
        } else if (method && !strcmp(method, "item/started")) {
            cx_item_event(c, params, 1, &fallback);
        } else if (method && !strcmp(method, "item/completed")) {
            cx_item_event(c, params, 0, &fallback);
        } else if (method && !strcmp(method, "rawResponseItem/completed")) {
            cx_raw_item_event(c, params);
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
    if (c->err_fd >= 0) close(c->err_fd);
    if (c->notice_fd[0] >= 0) close(c->notice_fd[0]);
    if (c->notice_fd[1] >= 0) close(c->notice_fd[1]);
    free(c->model); free(c->effort); free(c->sandbox); free(c->sys); free(c->resume);
    free(c->project); free(c->buf);
    if (c->warning_mu_ready) pthread_mutex_destroy(&c->warning_mu);
    free(c);
}

#endif /* CODEX_IMPLEMENTATION */
