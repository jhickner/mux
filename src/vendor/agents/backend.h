/*
 * backend.h — common backend interface for headless coding agents.
 *
 * Includes adapters for the nested claude/, codex/, grok/, and pi/ drivers. Define
 * BACKEND_IMPLEMENTATION in exactly one C file; compile it with one bundled
 * cJSON.c (for example, claude/cJSON.c).
 *
 *     #define BACKEND_IMPLEMENTATION
 *     #include "backend.h"
 *
 *     Backend *be = backend_open("pi", "openrouter/pareto-code", system);
 *     char *reply = be->ask(be, "fix the failing tests");
 *     free(reply);
 *     be->reset(be);       // discard conversation context
 *     be->close(be);
 *
 * backend_open_ex() takes the full option set and the vtable carries the rest of
 * what an interactive front end needs: an explicit start, streamed events, an
 * abort predicate, per-turn accounting, and the session id to resume from.
 */
#ifndef BACKEND_H
#define BACKEND_H

typedef struct Backend Backend;

/* Nothing here is retained: a backend copies what it needs. */
typedef struct {
    const char *name;           /* "claude" | "codex" | "grok" | "pi"; NULL -> claude */
    const char *model;          /* driver/CLI model identifier; NULL -> its default   */
    const char *system;         /* applied to every turn; NULL -> none                */
    const char *cwd;            /* where the agent runs its tools; NULL -> inherit    */
    const char *resume_session; /* continue a prior session (claude, codex, grok)     */
    const char *permission_mode;/* claude: --permission-mode; NULL -> bypassPermissions*/
    int allow_customizations;   /* claude: load skills, CLAUDE.md, MCP servers, ...   */
} backend_opts;

/* One interesting event from a turn's stream. Only the fields a kind documents
 * are set, and a driver that reports less leaves more of them NULL. */
typedef enum {
    BACKEND_EV_ASSISTANT,   /* text: an assistant text block                    */
    BACKEND_EV_THINKING,    /* text: a reasoning block                          */
    BACKEND_EV_TOOL,        /* name, plus input_json or arg: a tool invocation  */
    BACKEND_EV_TOOL_RESULT, /* text: the tool's output                          */
    BACKEND_EV_INIT,        /* name: the model the CLI resolved                 */
} backend_event_kind;

typedef struct {
    backend_event_kind kind;
    const char *text;
    const char *name;
    const char *input_json; /* tool input as compact JSON; NULL when the driver
                               reports no structured input                      */
    const char *arg;        /* the driver's own one-line rendering of the input,
                               for the drivers that have no input_json          */
} backend_event;

/* Accounting for one turn, zeroed before each. Token counts are that turn's;
 * a driver that does not report a field leaves it 0. */
typedef struct {
    double cost_usd;      /* cumulative for the session, as the CLI reports it */
    long   input_tokens;
    long   output_tokens;
    long   cache_read_tokens;
    long   cache_creation_tokens;
    long   context_tokens; /* latest primary-model request, including output */
    long   context_window;
    int    is_error;
    int    interrupted;   /* the abort predicate ended the turn */
} backend_result;

/* What a driver supports, in Backend.caps. */
#define BACKEND_CAP_RESUME 1u /* start() can adopt a prior session id */

struct Backend {
    unsigned caps;

    char *(*ask)(Backend *b, const char *user); /* malloc'd text or NULL */
    int   (*reset)(Backend *b);                 /* fresh context; nonzero on success */
    void  (*close)(Backend *b);                 /* frees b                           */

    /* Start (or restart) the child process now, adopting `resume_session` where
     * the driver supports it (NULL, or an unsupported driver, starts fresh), so
     * a missing CLI surfaces before the first turn. ask() starts lazily when
     * this was never called. Returns nonzero on success. */
    int (*start)(Backend *b, const char *resume_session);

    /* As ask(), also filling *meta, which is zeroed first. `meta` may be NULL. */
    char *(*ask_ex)(Backend *b, const char *user, backend_result *meta);

    /* Context occupancy as the driver knows it right now: during a turn this
     * tracks the latest model request rather than waiting for the turn to end,
     * so a caller painting a live display can follow it. Either output is 0
     * when unknown, and the whole hook is NULL for a driver that never reports
     * usage mid-turn. */
    void (*usage)(Backend *b, long *context_tokens, long *context_window);

    /* Takes effect at the next start(). */
    void (*set_model)(Backend *b, const char *model);

    /* Takes effect at the next start(). Ignored by the drivers with no
     * permission model of their own. */
    void (*set_permission)(Backend *b, const char *mode);

    /* Per-event sink for a turn, called on the ask() thread as events arrive;
     * the event and its strings are borrowed for the call. Pass NULL to clear. */
    void (*set_event_cb)(Backend *b, void (*cb)(void *ud, const backend_event *ev),
                         void *ud);

    /* Predicate polled while a turn is in flight; when it returns nonzero the
     * turn is abandoned. It is called every ~80-200ms, so it doubles as a tick
     * for a caller painting its own display. */
    void (*set_abort_check)(Backend *b, int (*cb)(void));

    /* NULL until known, or when the driver never reports it. */
    const char *(*session_id)(Backend *b);
    const char *(*model)(Backend *b);       /* the model the CLI resolved       */
    const char *(*auth_source)(Backend *b); /* "none" -> a subscription login   */
    const char *(*last_error)(Backend *b);  /* the tail of the child's stderr   */

    void *ctx;
};

/* Open "claude", "codex", "grok", or "pi". model is the driver/CLI model identifier
 * (NULL -> its default). system is applied to every turn (NULL -> none).
 * Returns NULL for an unknown name or allocation failure. */
Backend *backend_open(const char *name, const char *model, const char *system);

/* As backend_open, with the rest of the options. `opts` may be NULL. */
Backend *backend_open_ex(const backend_opts *opts);

/* The names backend_open accepts, NULL-terminated. */
const char *const *backend_names(void);

/* Fan n prompts across independent agent sessions. Each worker owns one
 * Backend, restarting its context after restart_every calls (0 -> never).
 * on_result calls are serialized; reply is valid only for the callback and is
 * freed afterward. Returns failed call count, or -1 for an unknown backend. */
int backend_run_pool(const char *name, const char *model, const char *system,
                     int workers, int restart_every,
                     const char **prompts, int n,
                     void (*on_result)(void *ud, int index, const char *reply),
                     void *ud);

#endif /* BACKEND_H */

/* ======================================================================== */
/*   IMPLEMENTATION                                                          */
/* ======================================================================== */
#ifdef BACKEND_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ---------- shared adapter scaffolding ----------
 *
 * Every adapter's context starts with a backend_state, so the option copies,
 * the event sink and the abort predicate are managed in one place. A driver's
 * client is created lazily and can be replaced (a restart, a model change), at
 * which point the callbacks are registered again from here.
 *
 * This block comes before the driver includes so that pi.h's own adapter can
 * use it too. */

typedef struct {
    char *model, *system, *cwd, *resume, *permission;
    int   allow_customizations;
    void (*on_event)(void *ud, const backend_event *ev);
    void *event_ud;
    int (*abort)(void);
    /* Assistant and reasoning text arriving in fragments, held until the block
     * it belongs to is complete. */
    backend_event_kind pending_kind;
    char  *pending;
    size_t pending_len, pending_cap;
} backend_state;

/* The kinds a driver streams a fragment at a time, for backend_emit_str. */
#define BACKEND_DELTA_ASSISTANT 1u
#define BACKEND_DELTA_THINKING  2u

static char *backend_dup(const char *s) { return (s && *s) ? strdup(s) : NULL; }

static void backend_set(char **slot, const char *value) {
    free(*slot);
    *slot = backend_dup(value);
}

static void backend_state_init(backend_state *st, const backend_opts *o) {
    st->model  = backend_dup(o->model);
    st->system = backend_dup(o->system);
    st->cwd    = backend_dup(o->cwd);
    st->resume = backend_dup(o->resume_session);
    st->permission = backend_dup(o->permission_mode);
    st->allow_customizations = o->allow_customizations;
}

static void backend_state_free(backend_state *st) {
    free(st->model); free(st->system); free(st->cwd); free(st->resume);
    free(st->permission); free(st->pending);
}

static void backend_emit(backend_state *st, const backend_event *ev) {
    if (st->on_event) st->on_event(st->event_ud, ev);
}

/* Release the accumulated text as one block. A caller that renders markdown or
 * wraps a line needs whole blocks, not the word-at-a-time deltas most of these
 * CLIs stream, so nothing reaches it until the block is closed: by an event of
 * another kind, or by the end of the turn. */
static void backend_flush(backend_state *st) {
    if (!st->pending_len) return;
    backend_event ev = { .kind = st->pending_kind, .text = st->pending };
    if (st->on_event) st->on_event(st->event_ud, &ev);
    st->pending_len = 0;
    st->pending[0] = '\0';
}

static void backend_delta(backend_state *st, backend_event_kind kind, const char *text) {
    if (!text || !*text) return;
    if (st->pending_len && st->pending_kind != kind) backend_flush(st);
    st->pending_kind = kind;
    size_t n = strlen(text);
    if (st->pending_len + n + 1 > st->pending_cap) {
        size_t cap = st->pending_cap ? st->pending_cap : 512;
        while (cap < st->pending_len + n + 1) cap *= 2;
        char *grown = realloc(st->pending, cap);
        if (!grown) return;
        st->pending = grown; st->pending_cap = cap;
    }
    memcpy(st->pending + st->pending_len, text, n + 1);
    st->pending_len += n;
}

/* Translate a driver's flat (kind, text) event. `tool_name` says what that
 * driver's "tool" events carry: NULL means text is the tool's own name,
 * otherwise text is its argument and tool_name names the tool. `deltas` is the
 * set of BACKEND_DELTA_* kinds this driver streams in fragments. */
static void backend_emit_str(backend_state *st, const char *kind, const char *text,
                             const char *tool_name, unsigned deltas) {
    if (!st->on_event || !kind) return;
    backend_event ev = {0};
    if (!strcmp(kind, "assistant")) {
        if (deltas & BACKEND_DELTA_ASSISTANT) { backend_delta(st, BACKEND_EV_ASSISTANT, text); return; }
        ev.kind = BACKEND_EV_ASSISTANT; ev.text = text;
    } else if (!strcmp(kind, "thinking")) {
        if (deltas & BACKEND_DELTA_THINKING) { backend_delta(st, BACKEND_EV_THINKING, text); return; }
        ev.kind = BACKEND_EV_THINKING; ev.text = text;
    } else if (!strcmp(kind, "tool-result")) {
        ev.kind = BACKEND_EV_TOOL_RESULT; ev.text = text;
    } else if (!strcmp(kind, "tool")) {
        ev.kind = BACKEND_EV_TOOL;
        ev.name = tool_name ? tool_name : text;
        ev.arg  = tool_name ? text : NULL;
    } else {
        return;
    }
    backend_flush(st);
    st->on_event(st->event_ud, &ev);
}

static void backend_set_model_generic(Backend *b, const char *model) {
    backend_set(&((backend_state *)b->ctx)->model, model);
}
static void backend_set_permission_generic(Backend *b, const char *mode) {
    backend_set(&((backend_state *)b->ctx)->permission, mode);
}
static void backend_set_permission_none(Backend *b, const char *mode) {
    (void)b; (void)mode;
}
static const char *backend_none(Backend *b) { (void)b; return NULL; }

#define CLAUDE_IMPLEMENTATION
#include "claude/claude.h"
#define CODEX_IMPLEMENTATION
#include "codex/codex.h"
#define GROK_IMPLEMENTATION
#include "grok/grok.h"
#define PI_IMPLEMENTATION
#define PI_BACKEND_IMPLEMENTATION
#include "pi/pi.h"

/* ---------- claude ---------- */

/* `live` is the turn's result block, written in place as events arrive, so it
 * doubles as the running usage report. The window only lands with the turn's
 * final event, so mid-turn it reads 0 and the caller falls back to whatever it
 * last saw. */
typedef struct { backend_state st; claude_client *client; claude_result live; } backend_claude;

static void backend_claude_event(void *ud, const claude_event *e) {
    backend_claude *x = ((Backend *)ud)->ctx;
    backend_event ev = { .text = e->text, .name = e->name, .input_json = e->input_json };
    switch (e->kind) {
    case CLAUDE_EV_ASSISTANT:   ev.kind = BACKEND_EV_ASSISTANT;   break;
    case CLAUDE_EV_THINKING:    ev.kind = BACKEND_EV_THINKING;    break;
    case CLAUDE_EV_TOOL:        ev.kind = BACKEND_EV_TOOL;        break;
    case CLAUDE_EV_TOOL_RESULT: ev.kind = BACKEND_EV_TOOL_RESULT; break;
    case CLAUDE_EV_INIT:        ev.kind = BACKEND_EV_INIT;        break;
    default: return;
    }
    backend_emit(&x->st, &ev);
}

static int backend_claude_start(Backend *b, const char *resume) {
    backend_claude *x = b->ctx;
    claude_opts o = {0};
    o.cwd = x->st.cwd;
    o.model = x->st.model;
    o.append_system = x->st.system;
    o.permission_mode = x->st.permission ? x->st.permission : "bypassPermissions";
    o.use_subscription = 1;
    o.allow_customizations = x->st.allow_customizations;
    o.resume_session = resume;
    claude_client *c = claude_start(&o);
    if (!c) return 0;
    claude_set_event_cb(c, backend_claude_event, b);
    claude_set_abort_check(c, x->st.abort);
    if (x->client) claude_stop(x->client);
    x->client = c;
    backend_set(&x->st.resume, resume);
    return 1;
}

static int backend_claude_ready(Backend *b) {
    backend_claude *x = b->ctx;
    return x->client || backend_claude_start(b, x->st.resume);
}

static char *backend_claude_ask_ex(Backend *b, const char *user, backend_result *meta) {
    backend_claude *x = b->ctx;
    if (meta) memset(meta, 0, sizeof *meta);
    if (!backend_claude_ready(b)) return NULL;
    claude_result *cr = &x->live;
    char *reply = claude_send_ex(x->client, user, cr);
    if (meta) {
        meta->cost_usd = cr->cost_usd;
        meta->input_tokens = cr->input_tokens;
        meta->output_tokens = cr->output_tokens;
        meta->cache_read_tokens = cr->cache_read_tokens;
        meta->cache_creation_tokens = cr->cache_creation_tokens;
        meta->context_tokens = cr->context_tokens;
        meta->context_window = cr->context_window;
        meta->is_error = cr->is_error;
        meta->interrupted = cr->interrupted;
    }
    return reply;
}

static void backend_claude_usage(Backend *b, long *tokens, long *window) {
    backend_claude *x = b->ctx;
    *tokens = x->live.context_tokens;
    *window = x->live.context_window;
}

static char *backend_claude_ask(Backend *b, const char *user) {
    return backend_claude_ask_ex(b, user, NULL);
}

static int backend_claude_reset(Backend *b) {
    backend_claude *x = b->ctx;
    if (!x->client) return backend_claude_ready(b);
    if (claude_reset(x->client)) return 1;
    claude_stop(x->client);
    x->client = NULL;
    return 0;
}

static void backend_claude_set_event_cb(Backend *b,
                                        void (*cb)(void *ud, const backend_event *ev),
                                        void *ud) {
    backend_claude *x = b->ctx;
    x->st.on_event = cb; x->st.event_ud = ud;
    if (x->client) claude_set_event_cb(x->client, backend_claude_event, b);
}

static void backend_claude_set_abort(Backend *b, int (*cb)(void)) {
    backend_claude *x = b->ctx;
    x->st.abort = cb;
    if (x->client) claude_set_abort_check(x->client, cb);
}

static const char *backend_claude_session_id(Backend *b) {
    backend_claude *x = b->ctx;
    return x->client ? claude_session_id(x->client) : NULL;
}
static const char *backend_claude_model(Backend *b) {
    backend_claude *x = b->ctx;
    return x->client ? claude_model(x->client) : NULL;
}
static const char *backend_claude_auth(Backend *b) {
    backend_claude *x = b->ctx;
    return x->client ? claude_auth_source(x->client) : NULL;
}
static const char *backend_claude_error(Backend *b) {
    backend_claude *x = b->ctx;
    return x->client ? claude_last_error(x->client) : NULL;
}

static void backend_claude_close(Backend *b) {
    backend_claude *x = b->ctx;
    if (x->client) claude_stop(x->client);
    backend_state_free(&x->st);
    free(x); free(b);
}

static Backend *backend_claude_open(const backend_opts *o) {
    backend_claude *x = calloc(1, sizeof *x);
    Backend *b = calloc(1, sizeof *b);
    if (!x || !b) { free(x); free(b); return NULL; }
    backend_state_init(&x->st, o);
    b->ctx = x;
    b->caps = BACKEND_CAP_RESUME;
    b->ask = backend_claude_ask;
    b->reset = backend_claude_reset;
    b->close = backend_claude_close;
    b->start = backend_claude_start;
    b->ask_ex = backend_claude_ask_ex;
    b->usage = backend_claude_usage;
    b->set_model = backend_set_model_generic;
    b->set_permission = backend_set_permission_generic;
    b->set_event_cb = backend_claude_set_event_cb;
    b->set_abort_check = backend_claude_set_abort;
    b->session_id = backend_claude_session_id;
    b->model = backend_claude_model;
    b->auth_source = backend_claude_auth;
    b->last_error = backend_claude_error;
    return b;
}

/* ---------- codex ---------- */

typedef struct { backend_state st; codex_client *client; } backend_codex;

/* Codex streams its answer a word at a time, but reports each reasoning summary
 * whole, and a command execution rather than a named tool call. */
static void backend_codex_event(void *ud, const char *kind, const char *text) {
    backend_codex *x = ((Backend *)ud)->ctx;
    backend_emit_str(&x->st, kind, text, "bash", BACKEND_DELTA_ASSISTANT);
}

static int backend_codex_start(Backend *b, const char *resume) {
    backend_codex *x = b->ctx;
    codex_opts o = {0};
    o.cwd = x->st.cwd;
    o.model = x->st.model;
    o.append_system = x->st.system;
    o.sandbox = "workspace-write";
    o.resume_session = resume;
    codex_client *c = codex_start(&o);
    if (!c) return 0;
    codex_set_event_cb(c, backend_codex_event, b);
    codex_set_abort_check(c, x->st.abort);
    if (x->client) codex_stop(x->client);
    x->client = c;
    backend_set(&x->st.resume, resume);
    return 1;
}

static char *backend_codex_ask(Backend *b, const char *user) {
    backend_codex *x = b->ctx;
    if (!x->client && !backend_codex_start(b, x->st.resume)) return NULL;
    char *reply = codex_send(x->client, user);
    backend_flush(&x->st);
    return reply;
}

static char *backend_codex_ask_ex(Backend *b, const char *user, backend_result *meta) {
    if (meta) memset(meta, 0, sizeof *meta);
    return backend_codex_ask(b, user);
}

static int backend_codex_reset(Backend *b) {
    backend_codex *x = b->ctx;
    if (!x->client) return backend_codex_start(b, NULL);
    codex_reset(x->client);
    return 1;
}

static void backend_codex_set_event_cb(Backend *b,
                                       void (*cb)(void *ud, const backend_event *ev),
                                       void *ud) {
    backend_codex *x = b->ctx;
    x->st.on_event = cb; x->st.event_ud = ud;
    if (x->client) codex_set_event_cb(x->client, backend_codex_event, b);
}

static void backend_codex_set_abort(Backend *b, int (*cb)(void)) {
    backend_codex *x = b->ctx;
    x->st.abort = cb;
    if (x->client) codex_set_abort_check(x->client, cb);
}

static const char *backend_codex_session_id(Backend *b) {
    backend_codex *x = b->ctx;
    return x->client ? codex_session_id(x->client) : NULL;
}

static void backend_codex_close(Backend *b) {
    backend_codex *x = b->ctx;
    if (x->client) codex_stop(x->client);
    backend_state_free(&x->st);
    free(x); free(b);
}

static Backend *backend_codex_open(const backend_opts *o) {
    backend_codex *x = calloc(1, sizeof *x);
    Backend *b = calloc(1, sizeof *b);
    if (!x || !b) { free(x); free(b); return NULL; }
    backend_state_init(&x->st, o);
    b->ctx = x;
    b->caps = BACKEND_CAP_RESUME;
    b->ask = backend_codex_ask;
    b->reset = backend_codex_reset;
    b->close = backend_codex_close;
    b->start = backend_codex_start;
    b->ask_ex = backend_codex_ask_ex;
    b->set_model = backend_set_model_generic;
    b->set_permission = backend_set_permission_none;
    b->set_event_cb = backend_codex_set_event_cb;
    b->set_abort_check = backend_codex_set_abort;
    b->session_id = backend_codex_session_id;
    b->model = backend_none;
    b->auth_source = backend_none;
    b->last_error = backend_none;
    return b;
}

/* ---------- grok ---------- */

typedef struct { backend_state st; grok_client *client; } backend_grok;

static void backend_grok_event(void *ud, const grok_event *e) {
    backend_grok *x = ((Backend *)ud)->ctx;
    switch (e->kind) {
    case GROK_EV_ASSISTANT:
        backend_delta(&x->st, BACKEND_EV_ASSISTANT, e->text);
        return;
    case GROK_EV_THINKING:
        backend_delta(&x->st, BACKEND_EV_THINKING, e->text);
        return;
    case GROK_EV_TOOL: {
        backend_event ev = {
            .kind = BACKEND_EV_TOOL,
            .name = e->name,
            .input_json = e->input_json,
            .arg = e->text,
        };
        backend_flush(&x->st);
        backend_emit(&x->st, &ev);
        return;
    }
    case GROK_EV_TOOL_RESULT: {
        backend_event ev = { .kind = BACKEND_EV_TOOL_RESULT, .text = e->text };
        backend_flush(&x->st);
        backend_emit(&x->st, &ev);
        return;
    }
    }
}

static int backend_grok_start(Backend *b, const char *resume) {
    backend_grok *x = b->ctx;
    grok_opts o = {0};
    o.cwd = x->st.cwd;
    o.model = x->st.model;
    o.append_system = x->st.system;
    o.reasoning_effort = getenv("GROK_EFFORT");
    o.resume_session = resume;
    grok_client *c = grok_start(&o);
    if (!c) return 0;
    grok_set_event_cb(c, backend_grok_event, b);
    grok_set_abort_check(c, x->st.abort);
    if (x->client) grok_stop(x->client);
    x->client = c;
    backend_set(&x->st.resume, resume);
    return 1;
}

static char *backend_grok_ask_ex(Backend *b, const char *user, backend_result *meta) {
    backend_grok *x = b->ctx;
    if (meta) memset(meta, 0, sizeof *meta);
    if (!x->client && !backend_grok_start(b, x->st.resume)) return NULL;
    grok_result gr = {0};
    char *reply = grok_send_ex(x->client, user, &gr);
    backend_flush(&x->st);
    if (meta) meta->interrupted = gr.interrupted;
    return reply;
}

static char *backend_grok_ask(Backend *b, const char *user) {
    return backend_grok_ask_ex(b, user, NULL);
}

/* There is no clear command, so a fresh context means a fresh process. */
static int backend_grok_reset(Backend *b) { return backend_grok_start(b, NULL); }

static void backend_grok_set_event_cb(Backend *b,
                                      void (*cb)(void *ud, const backend_event *ev),
                                      void *ud) {
    backend_grok *x = b->ctx;
    x->st.on_event = cb; x->st.event_ud = ud;
    if (x->client) grok_set_event_cb(x->client, backend_grok_event, b);
}

static void backend_grok_set_abort(Backend *b, int (*cb)(void)) {
    backend_grok *x = b->ctx;
    x->st.abort = cb;
    if (x->client) grok_set_abort_check(x->client, cb);
}

static const char *backend_grok_session_id(Backend *b) {
    backend_grok *x = b->ctx;
    return x->client ? grok_session_id(x->client) : NULL;
}

static void backend_grok_close(Backend *b) {
    backend_grok *x = b->ctx;
    if (x->client) grok_stop(x->client);
    backend_state_free(&x->st);
    free(x); free(b);
}

static Backend *backend_grok_open(const backend_opts *o) {
    backend_grok *x = calloc(1, sizeof *x);
    Backend *b = calloc(1, sizeof *b);
    if (!x || !b) { free(x); free(b); return NULL; }
    backend_state_init(&x->st, o);
    b->ctx = x;
    b->caps = BACKEND_CAP_RESUME;
    b->ask = backend_grok_ask;
    b->reset = backend_grok_reset;
    b->close = backend_grok_close;
    b->start = backend_grok_start;
    b->ask_ex = backend_grok_ask_ex;
    b->set_model = backend_set_model_generic;
    b->set_permission = backend_set_permission_none;
    b->set_event_cb = backend_grok_set_event_cb;
    b->set_abort_check = backend_grok_set_abort;
    b->session_id = backend_grok_session_id;
    b->model = backend_none;
    b->auth_source = backend_none;
    b->last_error = backend_none;
    return b;
}

/* ---------- dispatch ---------- */

static const char *const BACKEND_NAMES[] = { "claude", "codex", "grok", "pi", NULL };

const char *const *backend_names(void) { return BACKEND_NAMES; }

Backend *backend_open_ex(const backend_opts *opts) {
    backend_opts o = opts ? *opts : (backend_opts){0};
    if (!o.name || !strcmp(o.name, "claude")) return backend_claude_open(&o);
    if (!strcmp(o.name, "codex"))             return backend_codex_open(&o);
    if (!strcmp(o.name, "grok"))              return backend_grok_open(&o);
    if (!strcmp(o.name, "pi"))                return pi_backend_open(&o);
    return NULL;
}

Backend *backend_open(const char *name, const char *model, const char *system) {
    backend_opts o = { .name = name, .model = model, .system = system };
    return backend_open_ex(&o);
}

typedef struct {
    const char *name, *model, *system;
    int restart_every, next, failed, n;
    const char **prompts;
    void (*on_result)(void *ud, int index, const char *reply);
    void *ud;
    pthread_mutex_t queue_lock, result_lock;
} backend_pool;

static void *backend_pool_worker(void *arg) {
    backend_pool *p = arg;
    Backend *b = backend_open(p->name, p->model, p->system);
    if (!b) return NULL;
    int since_reset = 0;
    for (;;) {
        pthread_mutex_lock(&p->queue_lock);
        int index = p->next < p->n ? p->next++ : -1;
        pthread_mutex_unlock(&p->queue_lock);
        if (index < 0) break;
        if (p->restart_every > 0 && since_reset &&
            since_reset % p->restart_every == 0) b->reset(b);
        char *reply = b->ask(b, p->prompts[index]);
        since_reset++;
        pthread_mutex_lock(&p->result_lock);
        if (!reply) p->failed++;
        p->on_result(p->ud, index, reply);
        pthread_mutex_unlock(&p->result_lock);
        free(reply);
    }
    b->close(b);
    return NULL;
}

int backend_run_pool(const char *name, const char *model, const char *system,
                     int workers, int restart_every,
                     const char **prompts, int n,
                     void (*on_result)(void *ud, int index, const char *reply),
                     void *ud) {
    Backend *probe = backend_open(name, model, system);
    if (!probe) return -1;
    probe->close(probe);
    if (n <= 0) return 0;
    if (!prompts || !on_result) return n;
    if (workers < 1) workers = 1;
    if (workers > n) workers = n;
    backend_pool p = { .name = name, .model = model, .system = system,
                       .restart_every = restart_every, .prompts = prompts,
                       .n = n, .on_result = on_result, .ud = ud };
    pthread_mutex_init(&p.queue_lock, NULL);
    pthread_mutex_init(&p.result_lock, NULL);
    pthread_t *threads = malloc((size_t)workers * sizeof *threads);
    if (!threads) { pthread_mutex_destroy(&p.queue_lock); pthread_mutex_destroy(&p.result_lock); return n; }
    for (int i = 0; i < workers; i++) pthread_create(&threads[i], NULL, backend_pool_worker, &p);
    for (int i = 0; i < workers; i++) pthread_join(threads[i], NULL);
    free(threads);
    pthread_mutex_destroy(&p.queue_lock);
    pthread_mutex_destroy(&p.result_lock);
    return p.failed;
}

#endif /* BACKEND_IMPLEMENTATION */
