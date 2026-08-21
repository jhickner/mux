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
    const char *effort;          /* --effort value; NULL -> CLI default           */
    const char *permission_mode; /* --permission-mode (e.g. "bypassPermissions")  */
    const char *resume_session;  /* --resume <id> to continue a prior session     */
    int fork_session;            /* with resume_session: --fork-session, so the
                                    resumed context is copied into a new session
                                    id and the original transcript is untouched  */
    const char *append_system;   /* --append-system-prompt text; NULL -> none     */
    const char *session_name;    /* --name value; NULL -> let Claude name it      */
    const char *tools;           /* --tools value; NULL -> flag omitted (all tools);
                                    "" -> disable every built-in tool; else a list
                                    like "Bash,Read". Empty string is meaningful,
                                    so NULL (not "") is the "unset" sentinel.       */
    int use_subscription;        /* nonzero: unset ANTHROPIC_API_KEY in the child
                                    so the CLI uses your claude.ai login           */
    int no_session_persistence;  /* nonzero: pass --no-session-persistence         */
    int allow_customizations;    /* nonzero: drop --safe-mode, so the CLI loads the
                                    user's skills, CLAUDE.md, plugins, hooks, MCP
                                    servers, custom commands and agents. Zero (the
                                    {0} default) keeps the sandboxed behaviour.    */
} claude_opts;

/* Start a persistent headless claude process and prewarm it in the background.
 * Returns once the child and worker exist; the first operation waits if startup
 * is still in progress. */
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

/* Change the live session's effort with Claude Code's /effort command. NULL
 * selects "auto", Claude's model-default mode. */
int claude_set_effort(claude_client *c, const char *effort);

/* The model the CLI resolved: taken from the startup handshake's model table,
 * then from each turn's `system`/`init` event. Never waits, so it is NULL until
 * the handshake this client is already running has answered. */
const char *claude_model(claude_client *c);

/* The effort level in force: `--effort` if one was passed, else the
 * settings.json default, else whatever a later stream event reported.
 * NULL until one of those is known. */
const char *claude_effort(claude_client *c);

/* How the CLI authenticated, as it reports it: "none" means the claude.ai
 * login (what use_subscription asks for), otherwise the env var it used, e.g.
 * "ANTHROPIC_API_KEY". NULL until the first turn has been sent. */
const char *claude_auth_source(claude_client *c);

/* Run the CLI's claude.ai OAuth login, waiting until the browser flow finishes.
 * Concurrent callers share one successful login instead of opening several
 * browser windows. The abort predicate remains live while login is waiting. */
int claude_auth_login(claude_client *c);

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
    CLAUDE_EV_CWD,         /* text: the directory the session works in now     */
    CLAUDE_EV_TASK,        /* id + name (status) + text: a background task     */
} claude_event_kind;

typedef struct {
    claude_event_kind kind;
    const char *text;       /* NULL unless the kind documents it */
    const char *name;       /* tool name, model name for INIT, status for TASK */
    const char *input_json; /* tool input as compact JSON, for TOOL */
    const char *id;         /* TASK: the CLI's task id */
    const char *arg;        /* TASK: the subagent type, when it reports one */
    int failed;             /* TOOL_RESULT: the tool reported is_error */
} claude_event;

/* Register a callback invoked for each interesting stream event during a turn.
 * Runs on the claude_send thread as events arrive; the event and its strings are
 * borrowed for the duration of the call. Pass NULL to clear. Independent of
 * verbose (this is the programmatic sink; verbose is the stderr one). */
void claude_set_event_cb(claude_client *c,
                         void (*cb)(void *ud, const claude_event *ev),
                         void *ud);

/* How many background tasks the CLI currently has outstanding — subagents and
 * detached bash it started and has not yet been told about. A turn can end with
 * these still running, so a front end showing the agent as busy has to ask this
 * rather than take the turn's result as the end of the work. The CLI reports the
 * whole set whenever it changes, so this is a snapshot, not a running count. */
int claude_background_tasks(claude_client *c);

/* Ask the CLI to abandon the in-flight turn (the Agent SDK's interrupt control
 * request). The turn still ends with a result event, so the stream stays usable
 * for the next send. Returns nonzero on success. */
int claude_interrupt(claude_client *c);

/* Register an abort predicate, polled while a turn is in flight. When it first
 * returns nonzero the client interrupts the turn and keeps reading until the
 * CLI's result event, leaving the process ready for the next send. The predicate
 * doubles as a UI tick: it is called roughly every 80ms. */
void claude_set_abort_check(claude_client *c, int (*cb)(void));

/* The CLI runs turns of its own between sends: a finished background task or
 * subagent wakes the model with no prompt from here. Their events sit unread in
 * the pipe until the next send, which is both a lost display and a stream the
 * next send has to clear before it can trust what it reads.
 *
 * A front end that waits for input on a select() can watch claude_idle_fd() and
 * call claude_idle_pump() when it becomes readable, so those turns are shown as
 * they happen. Both are for the gap between sends only: the fd must never be
 * read directly, and the pump must not be called from the abort predicate or
 * any other point with a turn in flight.
 *
 * claude_idle_fd() returns -1 until the client is ready, and again once the
 * process is gone, so a caller can just ask before each wait. */
int claude_idle_fd(claude_client *c);

/* Consume whatever is readable now, dispatching events to the callback set by
 * claude_set_event_cb(). Never blocks. Returns nonzero while such a turn is
 * open or a background task it started is still running, so a caller can tell
 * "more is coming" from "that was all". */
int claude_idle_pump(claude_client *c);

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
#include <pthread.h>
#include <stdatomic.h>
#include <sys/wait.h>
#include "cJSON.h"

#define CLAUDE_ERR_MAX 4096

/* How long the turn loop waits on the child before running the abort predicate
 * again. Also the worst-case delay before a key the caller reads from that
 * predicate reaches the screen, so it sits below the ~30ms an echo can take
 * without being felt as lag. */
#define CL_TICK_MS 20

/* How long a stray turn may go silent before the client stops waiting it out
 * and sends anyway. Generous, because the silence is usually the model running
 * a slow tool; the fallback is only the misalignment this already avoided. */
#define CL_STRAY_QUIET_TICKS (60000 / CL_TICK_MS)

/* How long a task notification has to produce the turn it announces before the
 * client decides none is coming. Only the CLI's own dispatch latency, so short. */
#define CL_NOTIFY_WAIT_TICKS (2000 / CL_TICK_MS)

struct claude_client {
    pid_t pid;
    char  cli[4096];          /* executable used for this client and auth login */
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
    char  requested[128];     /* --model value, or the settings.json default */
    char  model[128];
    char  effort[32];
    char  auth[64];
    char  cwd[4096];          /* the CLI's working directory, per its init     */
    claude_result *meta;      /* filled from the in-flight turn's result event */
    int   turn_open;          /* an init has arrived with no result yet        */
    int   notified;           /* task notifications still owed a turn each     */
    int   bg_tasks;           /* background tasks the CLI last reported open   */
    int   awaiting;           /* a send is out and has not been given its turn */
    int   turn_mine;          /* the open turn answers this client's send      */
    char  stall[160];         /* why a send came back with no turn of its own  */
    char *buf;                /* line-assembly buffer for out_fd       */
    size_t len, cap;
    pthread_t warm_thread;
    int warm_joinable;
    atomic_int warm_state;    /* 0 while starting, 1 ready, -1 failed */
};

static void *cl_warm(void *arg);
static int cl_await_ready(claude_client *c);

void claude_set_verbose(claude_client *c, int on) { if (c) c->verbose = on; }

void claude_set_event_cb(claude_client *c,
                         void (*cb)(void *ud, const claude_event *ev),
                         void *ud) {
    if (c) { c->on_event = cb; c->on_event_ud = ud; }
}

const char *claude_model(claude_client *c) {
    return (c && c->model[0]) ? c->model : NULL;
}

const char *claude_effort(claude_client *c) {
    return (c && c->effort[0]) ? c->effort : NULL;
}

/* Copy one top-level string setting out of a settings.json, leaving `out`
 * untouched when the file, the parse or the key is missing. */
static void cl_read_setting(const char *path, const char *key, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long len = ftell(f);
    if (len <= 0 || len > 1 << 20) { fclose(f); return; }
    rewind(f);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return; }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) return;
    const char *val = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, key));
    if (val && *val)
        snprintf(out, n, "%s", val);
    cJSON_Delete(j);
}

/* The flag wins; otherwise take the setting from the user's settings, then a
 * project overlay. */
static void cl_seed_setting(const claude_opts *o, const char *flag,
                            const char *key, char *out, size_t n) {
    if (flag && *flag) {
        snprintf(out, n, "%s", flag);
        return;
    }
    if (!o->allow_customizations) return;
    const char *home = getenv("HOME");
    char path[512];
    if (home && *home) {
        snprintf(path, sizeof path, "%s/.claude/settings.json", home);
        cl_read_setting(path, key, out, n);
    }
    if (o->cwd && *o->cwd) {
        snprintf(path, sizeof path, "%s/.claude/settings.json", o->cwd);
        cl_read_setting(path, key, out, n);
    }
}

/* Stream events may replace the effort with the level the CLI resolved. */
static void cl_seed_effort(claude_client *c, const claude_opts *o) {
    cl_seed_setting(o, o->effort, "effortLevel", c->effort, sizeof c->effort);
}

/* The model as it was asked for — an alias like "sonnet", or "default" when
 * nothing selects one. The handshake's model table turns it into an id. */
static void cl_seed_model(claude_client *c, const claude_opts *o) {
    cl_seed_setting(o, o->model, "model", c->requested, sizeof c->requested);
    if (!c->requested[0])
        snprintf(c->requested, sizeof c->requested, "default");
}

/* The initialize response lists every selectable model with the id it resolves
 * to, which is what names the one in force before any turn has run. */
static void cl_note_models(claude_client *c, cJSON *response) {
    cJSON *models = cJSON_GetObjectItemCaseSensitive(response, "models");
    if (!cJSON_IsArray(models)) return;
    cJSON *m;
    cJSON_ArrayForEach(m, models) {
        const char *value = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(m, "value"));
        if (!value || strcmp(value, c->requested) != 0) continue;
        const char *id = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(m, "resolvedModel"));
        if (id && *id)
            snprintf(c->model, sizeof c->model, "%s", id);
        return;
    }
    /* The table lists the CLI's own selectors ("sonnet", "default"); a caller
     * may equally pass a concrete id, which the table has no entry for. That
     * value is what the CLI was handed, so it already names the model. */
    if (strcmp(c->requested, "default") != 0)
        snprintf(c->model, sizeof c->model, "%s", c->requested);
}

static void cl_note_effort(claude_client *c, cJSON *ev) {
    cJSON *e = cJSON_GetObjectItemCaseSensitive(ev, "effort");
    if (cJSON_IsString(e) && e->valuestring && e->valuestring[0]) {
        snprintf(c->effort, sizeof c->effort, "%s", e->valuestring);
        return;
    }
    if (!cJSON_IsObject(e)) return;
    const char *level = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(e, "level"));
    if (level && *level)
        snprintf(c->effort, sizeof c->effort, "%s", level);
}

/* Read whatever the child has written to stderr, keeping only the tail. Never
 * blocks: the fd is non-blocking. */
static void cl_append_error(claude_client *c, const char *text, size_t n) {
    if (!c || !text || !n) return;
    if (n >= CLAUDE_ERR_MAX) {          /* keep only the last chunk */
        memcpy(c->err, text + n - (CLAUDE_ERR_MAX - 1), CLAUDE_ERR_MAX - 1);
        c->err_len = CLAUDE_ERR_MAX - 1;
    } else {
        if (c->err_len + n > CLAUDE_ERR_MAX - 1) {
            size_t drop = c->err_len + n - (CLAUDE_ERR_MAX - 1);
            memmove(c->err, c->err + drop, c->err_len - drop);
            c->err_len -= drop;
        }
        memcpy(c->err + c->err_len, text, n);
        c->err_len += n;
    }
    c->err[c->err_len] = '\0';
}

static void cl_drain_error_fd(claude_client *c, int fd) {
    if (fd < 0) return;
    char tmp[1024];
    for (;;) {
        ssize_t r = read(fd, tmp, sizeof tmp);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return;
        }
        cl_append_error(c, tmp, (size_t)r);
    }
}

static void cl_drain_stderr(claude_client *c) {
    if (c) cl_drain_error_fd(c, c->err_fd);
}

const char *claude_last_error(claude_client *c) {
    if (!c) return NULL;
    cl_drain_stderr(c);
    /* Trim trailing newlines so callers can print it as one message. */
    while (c->err_len > 0 && (c->err[c->err_len - 1] == '\n' || c->err[c->err_len - 1] == '\r'))
        c->err[--c->err_len] = '\0';
    if (c->err_len) return c->err;
    /* A send that never got a turn says so itself: the CLI wrote nothing to
     * stderr, so there is nothing else to report it with. */
    return c->stall[0] ? c->stall : NULL;
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
        const char *argv[34];
        int n = 0;
        argv[n++] = cli;
        argv[n++] = "--print";
        argv[n++] = "--input-format";  argv[n++] = "stream-json";
        argv[n++] = "--output-format"; argv[n++] = "stream-json";
        argv[n++] = "--verbose";
        if (!o.allow_customizations) argv[n++] = "--safe-mode";
        if (o.no_session_persistence) argv[n++] = "--no-session-persistence";
        if (o.session_name && *o.session_name) { argv[n++] = "--name"; argv[n++] = o.session_name; }
        if (o.model && *o.model)           { argv[n++] = "--model";           argv[n++] = o.model; }
        if (o.effort && *o.effort)         { argv[n++] = "--effort";          argv[n++] = o.effort; }
        if (o.permission_mode && *o.permission_mode) { argv[n++] = "--permission-mode"; argv[n++] = o.permission_mode; }
        if (o.resume_session && *o.resume_session)   { argv[n++] = "--resume"; argv[n++] = o.resume_session;
                                                       if (o.fork_session) argv[n++] = "--fork-session"; }
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
    snprintf(c->cli, sizeof c->cli, "%s", cli);
    c->in_fd = in_pipe[1];
    c->out_fd = out_pipe[0];
    c->err_fd = err_pipe[0];
    cl_seed_effort(c, &o);
    cl_seed_model(c, &o);
    /* Never block the parent on the child's diagnostics. */
    fcntl(c->err_fd, F_SETFL, O_NONBLOCK);
    atomic_init(&c->warm_state, 0);
    if (!pthread_create(&c->warm_thread, NULL, cl_warm, c))
        c->warm_joinable = 1;
    else
        cl_warm(c);             /* rare resource failure: retain old semantics */
    return c;
}

void claude_set_abort_check(claude_client *c, int (*cb)(void)) {
    if (c) c->abort = cb;
}

const char *claude_session_id(claude_client *c) {
    if (!c || atomic_load_explicit(&c->warm_state, memory_order_acquire) != 1)
        return NULL;
    return c->session_id[0] ? c->session_id : NULL;
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
    case CLAUDE_EV_TASK:        return "task";
    case CLAUDE_EV_CWD:         return "cwd";
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
            out.failed = cJSON_IsTrue(cJSON_GetObjectItem(blk, "is_error"));
            out.text = t ? t : (out.failed ? "failed" : "(result)");
            cl_sink(c, &out);
        }
    }
}

static long cl_long(cJSON *obj, const char *key) {
    cJSON *v = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
    return (v && cJSON_IsNumber(v)) ? (long)v->valuedouble : 0;
}

/* Same model, allowing for the variant suffix that marks a context-window
 * choice: the CLI announces "claude-opus-5[1m]" in its init event and keys
 * modelUsage the same way, but the per-message model id drops the suffix. A
 * plain strcmp reads those as two models and throws the per-request usage away.
 */
static int cl_same_model(const char *a, const char *b) {
    size_t na = strcspn(a, "["), nb = strcspn(b, "[");
    return na == nb && strncmp(a, b, na) == 0;
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
    if (c->model[0] && model && !cl_same_model(model, c->model)) return;

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
        if (c->model[0] && entry->string && cl_same_model(entry->string, c->model))
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
    cl_note_effort(c, ev);
    cJSON *type = cJSON_GetObjectItemCaseSensitive(ev, "type");
    const char *ts = (type && cJSON_IsString(type)) ? type->valuestring : "";
    if (strcmp(ts, "system") == 0) {
        /* Every turn opens with an init and closes with a result, whether this
         * client asked for it or the CLI started it on its own. A finished
         * background task announces its turn with a notification first, so each
         * notification arriving between turns says one more turn is coming.
         *
         * Turns run one at a time and in the order they were asked for, so an
         * init can be attributed as it arrives: announced turns come first, and
         * the send waiting for a turn takes the first one nobody announced. */
        const char *sub = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "subtype"));
        if (sub && strcmp(sub, "init") == 0) {
            c->turn_open = 1;
            if (c->notified > 0) {
                c->notified--;
                c->turn_mine = 0;
            } else if (c->awaiting) {
                c->awaiting = 0;
                c->turn_mine = 1;
            } else {
                c->turn_mine = 0;
            }
        } else if (sub && strcmp(sub, "task_notification") == 0 && !c->turn_open) {
            c->notified++;
        } else if (sub && strcmp(sub, "background_tasks_changed") == 0) {
            /* The event carries the whole outstanding set, so it replaces the
             * count rather than adjusting it. */
            cJSON *tasks = cJSON_GetObjectItem(ev, "tasks");
            c->bg_tasks = cJSON_IsArray(tasks) ? cJSON_GetArraySize(tasks) : 0;
        }
        /* A background task -- a subagent, or a detached command -- reports its
         * own life cycle: task_started when it is launched, task_updated as its
         * status changes, task_notification with the summary it ends on. They
         * are the only word on work that outlives the turn that started it. */
        if (sub && !strncmp(sub, "task_", 5)) {
            cJSON *patch = cJSON_GetObjectItem(ev, "patch");
            claude_event out = {.kind = CLAUDE_EV_TASK};
            out.id = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "task_id"));
            out.name = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "status"));
            if (!out.name && patch)
                out.name = cJSON_GetStringValue(cJSON_GetObjectItem(patch, "status"));
            if (!out.name)
                out.name = !strcmp(sub, "task_started") ? "running" : "";
            out.arg = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "subagent_type"));
            out.text = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "description"));
            if (!out.text)
                out.text = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "summary"));
            if (out.id) cl_sink(c, &out);
        }
        const char *auth = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "apiKeySource"));
        if (auth && *auth)
            snprintf(c->auth, sizeof c->auth, "%s", auth);
        /* The init's cwd follows the session into and out of a worktree, so it
         * is not always the directory the client launched in. */
        const char *cwd = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "cwd"));
        if (cwd && *cwd && strcmp(cwd, c->cwd) != 0) {
            snprintf(c->cwd, sizeof c->cwd, "%s", cwd);
            claude_event out = {.kind = CLAUDE_EV_CWD, .text = c->cwd};
            cl_sink(c, &out);
        }
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
        /* Attributed when the turn opened; a result that never had an init of
         * its own answers whoever is waiting, since nothing announced it. */
        int mine = c->turn_open ? c->turn_mine : c->awaiting;
        c->turn_open = 0;
        c->turn_mine = 0;
        /* A turn the CLI ran on its own answers nobody here: its text is not a
         * reply to hand back, and its figures are not the caller's accounting. */
        if (mine) {
            c->awaiting = 0;
            cl_fill_result(c, ev);
            cJSON *res = cJSON_GetObjectItemCaseSensitive(ev, "result");
            const char *text = (res && cJSON_IsString(res)) ? res->valuestring : "";
            *out = strdup(text ? text : "");
        }
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

/* Read one complete JSONL message during startup. The worker is the only stdout
 * reader until initialization completes, so it can safely leave any following
 * bytes in the same buffer the normal turn loop uses. */
static int cl_warm_read(claude_client *c, cJSON **out) {
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
            memmove(c->buf, c->buf + used, c->len - used);
            c->len -= used;
            if (*out) return 1;
            continue;
        }

        struct pollfd pfds[2] = {
            { c->out_fd, POLLIN, 0 }, { c->err_fd, POLLIN, 0 }
        };
        int pr = poll(pfds, 2, CL_TICK_MS);
        if (pr < 0) { if (errno == EINTR) continue; return 0; }
        if (pfds[1].revents) cl_drain_stderr(c);
        if (!(pfds[0].revents & (POLLIN | POLLHUP))) continue;
        char tmp[8192];
        ssize_t r = read(c->out_fd, tmp, sizeof tmp);
        if (r <= 0) return 0;
        if (c->len + (size_t)r + 1 > c->cap) {
            size_t nc = (c->len + (size_t)r + 1) * 2;
            char *nb = realloc(c->buf, nc);
            if (!nb) return 0;
            c->buf = nb; c->cap = nc;
        }
        memcpy(c->buf + c->len, tmp, (size_t)r);
        c->len += (size_t)r;
        c->buf[c->len] = '\0';
    }
}

/* Claude's initialize control request forces skills, hooks, MCP configuration,
 * authentication, and the rest of CLI startup to finish without creating a
 * model turn. Run it while the prompt is already available to the user. */
static void *cl_warm(void *arg) {
    claude_client *c = arg;
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "control_request");
    cJSON_AddStringToObject(msg, "request_id", "claude_h_initialize");
    cJSON *req = cJSON_AddObjectToObject(msg, "request");
    cJSON_AddStringToObject(req, "subtype", "initialize");
    int ok = cl_write_json(c, msg);

    while (ok) {
        cJSON *ev = NULL;
        if (!cl_warm_read(c, &ev)) { ok = 0; break; }
        cl_note_session(c, ev);
        cl_note_effort(c, ev);
        const char *type = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(ev, "type"));
        cJSON *response = cJSON_GetObjectItemCaseSensitive(ev, "response");
        const char *request_id = response ? cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(response, "request_id")) : NULL;
        if (type && !strcmp(type, "control_response") && request_id &&
            !strcmp(request_id, "claude_h_initialize")) {
            const char *subtype = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(response, "subtype"));
            ok = subtype && !strcmp(subtype, "success");
            cJSON *inner = cJSON_GetObjectItemCaseSensitive(response, "response");
            if (ok && inner) cl_note_models(c, inner);
            cJSON_Delete(ev);
            break;
        }
        cJSON_Delete(ev);
    }
    atomic_store_explicit(&c->warm_state, ok ? 1 : -1, memory_order_release);
    return NULL;
}

static int cl_await_ready(claude_client *c) {
    if (!c) return 0;
    if (c->warm_joinable) {
        pthread_join(c->warm_thread, NULL);
        c->warm_joinable = 0;
    }
    return atomic_load_explicit(&c->warm_state, memory_order_acquire) == 1;
}

/* A burst of expired sessions should result in one browser login. Every caller
 * snapshots the generation before taking the lock; if another caller advanced
 * it while this one waited, the credential it just wrote is the one we need. */
static pthread_mutex_t cl_auth_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_ulong cl_auth_generation;

static int cl_wait_auth_child(claude_client *c, pid_t pid, int output_fd) {
    int status = 0;
    for (;;) {
        cl_drain_error_fd(c, output_fd);
        pid_t got = waitpid(pid, &status, WNOHANG);
        if (got == pid) break;
        if (got < 0 && errno != EINTR) return 0;
        if (c->abort && c->abort()) {
            kill(pid, SIGTERM);
            for (int waited = 0; waited < 250; waited += 5) {
                if (waitpid(pid, &status, WNOHANG) == pid) return 0;
                usleep(5000);
            }
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return 0;
        }
        usleep(CL_TICK_MS * 1000);
    }
    cl_drain_error_fd(c, output_fd);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int claude_auth_login(claude_client *c) {
    if (!c || !c->cli[0]) return 0;

    unsigned long seen = atomic_load_explicit(&cl_auth_generation, memory_order_acquire);
    pthread_mutex_lock(&cl_auth_lock);
    if (atomic_load_explicit(&cl_auth_generation, memory_order_relaxed) != seen) {
        pthread_mutex_unlock(&cl_auth_lock);
        return 1;
    }

    int output[2];
    if (pipe(output) != 0) {
        pthread_mutex_unlock(&cl_auth_lock);
        return 0;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(output[0]);
        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            if (null_fd != STDIN_FILENO) close(null_fd);
        }
        dup2(output[1], STDOUT_FILENO);
        dup2(output[1], STDERR_FILENO);
        if (output[1] != STDOUT_FILENO && output[1] != STDERR_FILENO)
            close(output[1]);
        unsetenv("ANTHROPIC_API_KEY");
        const char *argv[] = {c->cli, "auth", "login", "--claudeai", NULL};
        execvp(c->cli, (char *const *)argv);
        _exit(127);
    }
    close(output[1]);
    if (pid < 0) {
        close(output[0]);
        pthread_mutex_unlock(&cl_auth_lock);
        return 0;
    }

    fcntl(output[0], F_SETFL, O_NONBLOCK);
    c->err_len = 0;
    c->err[0] = '\0';
    int ok = cl_wait_auth_child(c, pid, output[0]);
    close(output[0]);
    if (ok) {
        c->err_len = 0;
        c->err[0] = '\0';
        atomic_fetch_add_explicit(&cl_auth_generation, 1, memory_order_release);
    }
    pthread_mutex_unlock(&cl_auth_lock);
    return ok;
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

/* Wait up to timeout_ms for the child and append whatever arrives to the line
 * buffer. Returns 1 when bytes were added, 0 on timeout, -1 once the process is
 * gone or the buffer cannot grow — either way the buffer is dropped. */
static int cl_fill(claude_client *c, int timeout_ms) {
    struct pollfd pfds[2] = { { c->out_fd, POLLIN, 0 }, { c->err_fd, POLLIN, 0 } };
    int pr = poll(pfds, 2, timeout_ms);
    if (pr < 0) {
        if (errno == EINTR) return 0;
        c->len = 0;
        return -1;
    }
    if (pfds[1].revents) cl_drain_stderr(c);     /* keep the pipe from filling */
    if (!(pfds[0].revents & (POLLIN | POLLHUP))) return 0;

    char tmp[8192];
    ssize_t r = read(c->out_fd, tmp, sizeof tmp);
    if (r <= 0) { c->len = 0; return -1; }       /* EOF / error: process gone */
    if (c->len + (size_t)r + 1 > c->cap) {
        size_t nc = (c->len + (size_t)r + 1) * 2;
        char *nb = realloc(c->buf, nc);
        if (!nb) { c->len = 0; return -1; }
        c->buf = nb; c->cap = nc;
    }
    memcpy(c->buf + c->len, tmp, (size_t)r);
    c->len += (size_t)r;
    c->buf[c->len] = '\0';
    return 1;
}

/* Handle the complete lines sitting in the buffer, stopping after a turn's
 * result (its text lands in *out, untouched otherwise) so the bytes of any
 * following turn stay queued. Returns how many lines were handled. */
static int cl_scan_lines_n(claude_client *c, char **out, int max) {
    if (!c->buf || !c->len || max <= 0) return 0;
    int lines = 0;
    char *start = c->buf, *nl;
    while (lines < max && (nl = memchr(start, '\n', c->len - (size_t)(start - c->buf)))) {
        *nl = '\0';
        lines++;
        int done = cl_handle_line(c, start, out);
        start = nl + 1;
        if (done) break;
    }
    size_t consumed = (size_t)(start - c->buf);
    if (consumed) { memmove(c->buf, start, c->len - consumed); c->len -= consumed; }
    return lines;
}

static int cl_scan_lines(claude_client *c, char **out) {
    return cl_scan_lines_n(c, out, 1000000);
}

/* Consume any turn the CLI ran on its own — a finished background task or
 * subagent wakes the model with no send from here, and its events sit in the
 * pipe until someone reads them. Taking that turn's result as the answer to the
 * next send shifts every reply by one turn and returns before the caller's
 * spinner can appear, so the stream is cleared first. The stray turn's figures
 * belong to no caller: c->meta stays out of it. */
static void cl_drain_stray(claude_client *c) {
    claude_result *saved = c->meta;
    c->meta = NULL;
    int interrupted = 0, quiet = 0;
    for (;;) {
        char *stray = NULL;
        int lines = cl_scan_lines(c, &stray);
        free(stray);
        if (lines) { quiet = 0; continue; }  /* keep going while the buffer has more */

        /* Waiting out an announced turn as well as an open one closes the gap
         * where a notification has landed but its turn has not started yet —
         * returning in that gap would leave the stray turn to collide with the
         * send this drain is clearing the way for. */
        int waiting = c->turn_open || c->notified;
        if (c->turn_open && c->abort && !interrupted && c->abort()) {
            interrupted = 1;
            claude_interrupt(c);
        }
        int r = cl_fill(c, waiting ? CL_TICK_MS : 0);
        if (r < 0) break;                        /* process gone */
        if (r > 0) { quiet = 0; continue; }
        if (!waiting) break;                     /* no turn open or owed: clean */
        if (++quiet > (c->turn_open ? CL_STRAY_QUIET_TICKS : CL_NOTIFY_WAIT_TICKS))
            break;
    }
    /* An announcement that never produced a turn must not arm the next drain. */
    if (!c->turn_open) c->notified = 0;
    c->meta = saved;
}

int claude_idle_fd(claude_client *c) {
    if (!c || c->pid <= 0) return -1;
    /* The warm thread is the only stdout reader until it publishes readiness. */
    if (atomic_load_explicit(&c->warm_state, memory_order_acquire) != 1) return -1;
    return c->out_fd;
}

int claude_idle_pump(claude_client *c) {
    if (claude_idle_fd(c) < 0) return 0;
    claude_result *saved = c->meta;
    c->meta = NULL;                   /* these turns are nobody's accounting */
    char *stray = NULL;
    int lines = cl_scan_lines_n(c, &stray, 16);
    free(stray);
    if (!lines && cl_fill(c, 0) > 0) {
        stray = NULL;
        cl_scan_lines_n(c, &stray, 16);
        free(stray);
    }
    c->meta = saved;
    /* Deliberately not c->notified: a task that ends without waking the model —
     * one stopped at shutdown, say — is announced but never gets its turn, and
     * counting it here would leave the caller showing work that never lands.
     * The next send's drain is where an unanswered announcement is written off. */
    return c->turn_open || c->bg_tasks;
}

int claude_background_tasks(claude_client *c) { return c ? c->bg_tasks : 0; }

static char *cl_send(claude_client *c, const char *user_text, int content_block) {
    if (!c || !user_text) return NULL;
    if (!cl_await_ready(c)) return NULL;
    cl_drain_stray(c);

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
    c->stall[0] = '\0';
    c->awaiting = 1;
    if (!cl_write_json(c, msg)) { c->awaiting = 0; return NULL; }

    /* Read events until this turn's result. Turns the CLI started on its own
     * still stream through the event callback, but their results are not this
     * send's answer, so the loop reads past them. */
    char *result = NULL;
    int interrupted = 0, quiet = 0;
    for (;;) {
        /* Abort mid-turn: ask the CLI to abandon the turn, then keep reading to
         * its result event so the stream stays aligned for the next send. */
        if (c->abort && !interrupted && c->abort()) {
            interrupted = 1;
            claude_interrupt(c);
            if (c->meta) c->meta->interrupted = 1;
        }
        /* The abort predicate doubles as a UI tick, and a caller that echoes
         * typing from it cannot answer a keystroke sooner than this timeout, so
         * it is set by what feels immediate rather than by the spinner. */
        int r = cl_fill(c, CL_TICK_MS);
        if (r < 0) break;
        int lines = cl_scan_lines(c, &result);
        if (result) { c->awaiting = 0; return result; }
        if (r > 0 || lines) { quiet = 0; continue; }

        /* The turn this send is owed opens within a tick of the write, so going
         * quiet with none open and none arriving means the CLI folded the send
         * into a turn that was already running — whose result was read past as
         * somebody else's. Nothing more is coming: end the turn on what the
         * stream already showed rather than wait forever. */
        /* Reported as a failure rather than as an empty answer: the send was
         * never given a turn, so there is no reply -- which is a different
         * thing from a turn that ran and said nothing. */
        if (!c->turn_open && ++quiet > CL_STRAY_QUIET_TICKS) {
            c->awaiting = 0;
            snprintf(c->stall, sizeof c->stall,
                     "the CLI never opened a turn for this message (%d seconds "
                     "of silence)", (CL_STRAY_QUIET_TICKS * CL_TICK_MS) / 1000);
            return NULL;
        }
    }
    c->awaiting = 0;
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

int claude_set_effort(claude_client *c, const char *effort) {
    if (!c) return 0;
    const char *level = effort && *effort ? effort : "auto";
    size_t n = strlen(level) + sizeof "/effort ";
    char *command = malloc(n);
    if (!command) return 0;
    snprintf(command, n, "/effort %s", level);
    char *result = cl_send(c, command, 1);
    free(command);
    int ok = result && strncmp(result, "Invalid argument:", 17) != 0;
    free(result);
    if (ok)
        snprintf(c->effort, sizeof c->effort, "%s", level);
    return ok;
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
    if (c->warm_joinable) {
        pthread_join(c->warm_thread, NULL);
        c->warm_joinable = 0;
    }
    if (c->out_fd >= 0) close(c->out_fd);
    if (c->err_fd >= 0) close(c->err_fd);
    free(c->buf);
    free(c);
}

#endif /* CLAUDE_IMPLEMENTATION */
