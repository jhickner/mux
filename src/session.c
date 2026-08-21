#include "session.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "agenttabs.h"
#include "block.h"
#include "app.h"
#include "prompt.h"
#include "filediff.h"
#include "gitinfo.h"
#include "hud.h"
#include "image.h"
#include "livelist.h"
#include "restart.h"
#include "md.h"
#include "sessionprefs.h"
#include "sessionview.h"
#include "viewport.h"
#include "settings.h"
#include "sidechannel.h"
#include "status.h"
#include "title.h"
#include "tg.h"
#include "toolstyle.h"
#include "transcript.h"
#include "tty.h"
#include "ui.h"
#include "vendor/agents/backend.h"
#include "text.h"
#include "vendor/cJSON.h"

struct session {
    Backend *agent;
    char    *backend;
    char    *cwd;
    char    *workdir;
    char    *model;
    char    *effort;
    char    *resolved;
    char     id[128];
    char     title[128];
    char     stale_title[128];
    int      announce_title;
    int      retitle;
    int      named;
    double   named_at;
    char    *prompt;
    char    *last_reply;
    char    *failed_prompt;
    struct transcript transcript;
    char    *last_block;
    char    *streamed;
    size_t   streamed_len, streamed_cap;
    int      turns;
    double   cost_usd;
    long     context_tokens;
    long     context_window;
    int      quiet;
    int      silent;
    char    *system_extra;
    session_event_fn observer;
    void    *observer_ud;
    int    (*abort_hook)(void *ud);
    void    *abort_ud;
    int      skip_naming;
    int      thinking;
    int      compact;
    int      customizations;
    int      fork_session;
    char    *permission;
    char    *error_note;
    int      idle_busy;
    int      trust_requested;
    int      interrupted;
    int      unseen;        /* a turn ended behind whoever is holding this */

    // A turn running on its own thread. Everything it produces is copied into
    // `queue` and drawn later by whoever owns the screen.
    pthread_t       thread;
    int             running;
    volatile int    finished;
    volatile int    abort_request;
    pthread_mutex_t lock;
    struct evcopy  *head, *tail;
    int             wake[2];
    char           *asked;
    char           *reply;
    backend_result  meta;
    double          started;

    struct turnview view;
};

// One queued event, with every string it borrowed copied.
struct evcopy {
    backend_event  ev;
    char          *text, *name, *input_json, *arg, *diff, *id;
    struct evcopy *next;
};

static void replace(char **slot, const char *value)
{
    free(*slot);
    *slot = value ? strdup(value) : NULL;
}

static void stream_append(struct session *s, const char *value)
{
    if (!value || !*value)
        return;
    size_t add = strlen(value);
    if (s->streamed_len + add + 1 > s->streamed_cap) {
        size_t want = s->streamed_cap ? s->streamed_cap : 1024;
        while (want < s->streamed_len + add + 1)
            want *= 2;
        char *grown = realloc(s->streamed, want);
        if (!grown)
            return;
        s->streamed = grown;
        s->streamed_cap = want;
    }
    memcpy(s->streamed + s->streamed_len, value, add + 1);
    s->streamed_len += add;
}

static void stream_reset(struct session *s)
{
    free(s->streamed);
    s->streamed = NULL;
    s->streamed_len = s->streamed_cap = 0;
}

static void humanize(long n, char *out, size_t size)
{
    if (n < 1000)
        snprintf(out, size, "%ld", n);
    else if (n < 1000000)
        snprintf(out, size, "%.1fk", (double)n / 1000.0);
    else
        snprintf(out, size, "%.1fM", (double)n / 1000000.0);
}

// The session the drawing belongs to. One writer discipline: every setter
// swaps it and puts back what it found, so a nested pump cannot leave the
// screen owned by nobody.
static struct session *live;

struct session *session_set_drawing(struct session *s)
{
    struct session *was = live;
    live = s;
    return was;
}

static void remember_model(const struct session *s);
static int dir_alive(const char *path);
static int ground_target(const char *gone, char *out, size_t size);
static void await_model(struct session *s);

// Drawing, and the state that goes with it. Only ever called by whoever owns
// the screen: for a threaded turn that is the pump, not the turn.
static void render_event(struct session *s, const backend_event *ev)
{
    if (ev->kind == BACKEND_EV_INIT) {
        replace(&s->resolved, ev->name);
        return;
    }

    if (ev->kind == BACKEND_EV_CWD) {
        if (ev->text && *ev->text)
            replace(&s->workdir, ev->text);
        return;
    }

    if (ev->kind == BACKEND_EV_TRUST) {
        s->trust_requested = 1;
        return;
    }

    // Kept even when nothing is being drawn: a turn that ends without a reply
    // still has this to fall back on.
    if (ev->kind == BACKEND_EV_ASSISTANT && ev->text && *ev->text)
        replace(&s->last_block, ev->text);

    if (s->observer)
        s->observer(s->observer_ud, ev);

    if (s->quiet || s->silent || live != s)
        return;

    int paused = 0;

    switch (ev->kind) {
    case BACKEND_EV_INIT:
    case BACKEND_EV_CWD:
    case BACKEND_EV_TRUST:
    case BACKEND_EV_TASK:
        break;

    case BACKEND_EV_WARNING:
        if (ev->text && *ev->text) {
            status_pause();
            paused = 1;
            ui_note("%s", ev->text);
        }
        break;

    case BACKEND_EV_ASSISTANT:
        if (!ev->text || !*ev->text)
            break;
        status_pause();
        paused = 1;

        md_render_kept(ev->text, 0);
        stream_append(s, ev->text);
        view_cluster_forget(&s->view);
        s->view.after_activity = 0;
        s->view.after_tool = 0;
        s->view.after_collapse = 0;
        break;

    case BACKEND_EV_THINKING:
        if (!s->thinking || !ev->text || !*ev->text)
            break;
        status_pause();
        paused = 1;
        view_cluster_forget(&s->view);
        view_keep_activity("\xe2\x9c\xbb", ev->text, UI_THINKING, s->view.after_tool);
        s->view.after_activity = 1;
        s->view.after_tool = 1;
        s->view.after_collapse = 0;
        break;

    case BACKEND_EV_TOOL: {
        const char *name = ev->name ? ev->name : "?";
        char arg[4096];
        view_tool_argument(ev, s->cwd, arg, sizeof arg);
        int collapsed = s->compact || toolstyle_collapses(name, ev->input_json, ev->arg);

        status_pause();
        paused = 1;
        if (collapsed) {
            if (!view_cluster_extend(&s->view, name, arg))
                view_cluster_start(&s->view, name, arg,
                                   s->view.after_tool && !s->view.after_collapse);
            view_cluster_paint(&s->view);
        } else {
            view_cluster_forget(&s->view);
            view_keep_tool_call(name, arg, s->view.after_tool);
        }

        char path[4096];
        if (!collapsed && view_tool_path(ev->input_json, s->cwd, path, sizeof path))
            filediff_snapshot(path);
        else
            filediff_clear();

        s->view.after_activity = 1;
        s->view.after_tool = 1;
        s->view.after_collapse = collapsed;
        break;
    }

    case BACKEND_EV_TOOL_RESULT:
        if (ev->failed) {
            status_pause();
            paused = 1;
            view_cluster_forget(&s->view);
            filediff_clear();
            {
                const char *why = ev->text && *ev->text ? ev->text : NULL;
                if (!why || !strcmp(why, "failed")) {
                    view_keep_output("failed", UI_ERROR, 0);
                } else {
                    char line[4096];
                    snprintf(line, sizeof line, "failed: %s", why);
                    view_keep_output(line, UI_ERROR, 1);
                }
            }
            s->view.after_activity = 1;
            s->view.after_tool = 1;
            s->view.after_collapse = 0;
            break;
        }

        if (!s->view.after_collapse) {
            status_pause();
            paused = 1;

            // The patch is kept, not the rows it drew.
            char *patch;
            if (ev->diff) {
                patch = strdup(ev->diff);
                filediff_clear();
            } else {
                patch = filediff_take_patch();
            }
            if (filediff_patch_draws(patch)) {
                view_keep_diff(patch);
            } else {
                free(patch);
                view_keep_output(ev->text, UI_DIM, 0);
            }
        }
        s->view.after_activity = 1;
        s->view.after_tool = 1;
        break;
    }

    if (paused)
        status_resume();
    ui_flush();
}

static void wake_write(struct session *s);

static char *dup_or_null(const char *s)
{
    return s ? strdup(s) : NULL;
}

static void evcopy_free(struct evcopy *e)
{
    free(e->text);
    free(e->name);
    free(e->input_json);
    free(e->arg);
    free(e->diff);
    free(e->id);
    free(e);
}

// Called on the turn's thread: the event and its strings are borrowed for the
// call, so what is kept is a copy.
static void enqueue(struct session *s, const backend_event *ev)
{
    struct evcopy *e = calloc(1, sizeof *e);
    if (!e)
        return;
    e->ev = *ev;
    e->ev.text = e->text = dup_or_null(ev->text);
    e->ev.name = e->name = dup_or_null(ev->name);
    e->ev.input_json = e->input_json = dup_or_null(ev->input_json);
    e->ev.arg = e->arg = dup_or_null(ev->arg);
    e->ev.diff = e->diff = dup_or_null(ev->diff);
    e->ev.id = e->id = dup_or_null(ev->id);

    pthread_mutex_lock(&s->lock);
    if (s->tail)
        s->tail->next = e;
    else
        s->head = e;
    s->tail = e;
    pthread_mutex_unlock(&s->lock);

    // Whatever the front end is waiting in has to come up for air.
    wake_write(s);
}

static struct evcopy *dequeue(struct session *s)
{
    pthread_mutex_lock(&s->lock);
    struct evcopy *e = s->head;
    if (e) {
        s->head = e->next;
        if (!s->head)
            s->tail = NULL;
    }
    pthread_mutex_unlock(&s->lock);
    return e;
}

static void queue_drop(struct session *s)
{
    struct evcopy *e;
    while ((e = dequeue(s)))
        evcopy_free(e);
}

static void on_event(void *ud, const backend_event *ev)
{
    struct session *s = ud;
    if (s->running) {
        enqueue(s, ev);
        return;
    }
    render_event(s, ev);
}

int session_wake_fd(const struct session *s)
{
    return s ? s->wake[0] : -1;
}

int session_idle_fd(const struct session *s)
{
    if (!s || !s->agent || !s->agent->idle_fd || s->quiet)
        return -1;
    return s->agent->idle_fd(s->agent);
}

// The two registries a status change goes to: the tmux tab record for the
// window, and this session's entry in the list every mux can read.
static void publish(const struct session *s, const char *status)
{
    if (!strcmp(status, "working"))
        agenttabs_working();
    else if (!strcmp(status, "errored"))
        agenttabs_errored();
    else
        agenttabs_finished();
    livelist_publish(s, status);
}

static void tab_busy(struct session *s, int busy)
{
    busy = busy ? 1 : 0;
    if (busy == s->idle_busy)
        return;
    s->idle_busy = busy;
    publish(s, busy ? "working" : "finished");
}

static void name_poll(struct session *s);

int session_idle_pump(struct session *s)
{
    if (!s || !s->agent)
        return 0;

    // A name asked for at the prompt lands here: nothing else polls for it
    // between turns.
    name_poll(s);

    if (!s->agent->idle_pump || s->quiet)
        return 0;

    if (!s->idle_busy) {
        view_cluster_forget(&s->view);
        s->view.after_activity = 0;
        s->view.after_tool = 0;
        s->view.after_collapse = 0;
    }
    struct session *was = session_set_drawing(s);
    image_poll();
    int busy = s->agent->idle_pump(s->agent) ? 1 : 0;
    session_set_drawing(was);

    tab_busy(s, busy);
    return busy;
}

int session_idle_busy(const struct session *s)
{
    if (!s || !s->agent || !s->agent->busy)
        return 0;
    return s->agent->busy(s->agent) ? 1 : 0;
}

static session_key_fn typeahead;
static void          *typeahead_ud;

void session_set_typeahead(session_key_fn fn, void *ud)
{
    typeahead = fn;
    typeahead_ud = ud;
}

static void set_id(struct session *s, const char *id);

static void quota_poll(struct session *s)
{
    if (!s || !s->agent || !s->agent->rate_limit)
        return;
    backend_rate_limit limit = {0};
    s->agent->rate_limit(s->agent, &limit);
    if (limit.available)
        agenttabs_usage(limit.used_percent, limit.resets_at, limit.window_minutes);
}

// Takes the cached name, unless it is the one a rename is replacing.
static int adopt_title(struct session *s)
{
    char found[sizeof s->title];
    if (!title_lookup(s->id, found, sizeof found))
        return 0;
    if (s->stale_title[0] && !strcmp(found, s->stale_title))
        return 0;
    s->stale_title[0] = '\0';
    snprintf(s->title, sizeof s->title, "%s", found);
    status_set_note(s->title);
    publish(s, s->idle_busy ? "working" : "finished");
    if (s->announce_title) {
        s->announce_title = 0;
        ui_block_begin(1, 1);
        ui_note("renamed to %s", s->title);
        ui_block_end();
    }
    return 1;
}

static void name_poll(struct session *s)
{
    if (!s || s->title[0] || !s->agent)
        return;

    if (!s->id[0]) {
        const char *id = s->agent->session_id(s->agent);
        if (!id)
            return;
        set_id(s, id);
        if (s->title[0] || !s->id[0])
            return;
    }

    if (!s->named) {
        if (s->skip_naming || !s->prompt)
            return;
        const char *model = s->resolved && *s->resolved ? s->resolved : s->model;
        title_request(s->id, s->backend, model, s->cwd, s->prompt, s->last_reply);
        s->named = 1;
        s->retitle = 0;
        s->named_at = now_seconds();
        return;
    }

    double now = now_seconds();
    if (now - s->named_at < 1.0)
        return;
    s->named_at = now;
    // The name is what the other windows list this session by.
    adopt_title(s);
}

int session_rename(struct session *s, const char *name)
{
    if (!s || !s->id[0])
        return 0;

    if (name && *name) {
        if (!title_set(s->id, name))
            return 0;
        s->stale_title[0] = '\0';
        if (!adopt_title(s))
            return 0;
        s->named = 1;
        s->retitle = 0;
        return 1;
    }

    if (!s->prompt)
        return 0;

    const char *model = s->resolved && *s->resolved ? s->resolved : s->model;
    title_request(s->id, s->backend, model, s->cwd, s->prompt, s->last_reply);
    snprintf(s->stale_title, sizeof s->stale_title, "%s", s->title);
    s->title[0] = '\0';
    s->named = 1;
    s->retitle = 0;
    s->named_at = now_seconds();
    s->announce_title = 1;
    status_set_note(NULL);
    return 1;
}

static int effort_is_off(const char *effort)
{
    return !effort || !*effort ||
           !strcmp(effort, "none") || !strcmp(effort, "off");
}

static const char *spin_effort(const struct session *s)
{
    if (s->effort && *s->effort &&
        strcmp(s->effort, "default") != 0 && strcmp(s->effort, "auto") != 0)
        return s->effort;
    if (s->agent && s->agent->effort) {
        const char *got = s->agent->effort(s->agent);
        if (got && *got && strcmp(got, "auto") != 0)
            return got;
    }
    return NULL;
}

static void set_spin_word(const struct session *s)
{
    const char *effort = spin_effort(s);
    if (effort_is_off(effort)) {
        status_set_word("working");
        return;
    }
    char phrase[64];
    snprintf(phrase, sizeof phrase, "thinking with %s effort", effort);
    status_set_word(phrase);
}

int session_poll_input(void)
{
    if (!tty_is_raw())
        return 0;

    int interrupt = 0;
    tty_event ev;
    while (tty_read(&ev, 0)) {

        status_touch();
        if (typeahead) {
            interrupt |= typeahead(typeahead_ud, &ev);
            continue;
        }
        if (ev.key == TK_TEXT)
            free(ev.text);
        if (ev.key == TK_ESCAPE || (ev.key == TK_CHAR && ev.cp == 3))
            interrupt = 1;
        if (ev.key == TK_EOF)
            interrupt = 1;
    }
    return interrupt;
}

// The turn a thread is running, so the one abort predicate the backend offers
// can tell whose turn is asking. NULL on the main thread, which is where the
// blocking turns still run.
static __thread struct session *owner;

static int abort_check(void)
{
    if (owner)
        return owner->abort_request;

    name_poll(live);
    quota_poll(live);
    if (live)
        set_spin_word(live);

    int interrupt = session_poll_input();
    if (live && live->abort_hook)
        interrupt |= live->abort_hook(live->abort_ud);

    sidechannel_poll();
    sidechannel_tick();
    image_poll();
    status_tick();
    return interrupt;
}

struct session *session_new(const char *backend, const char *cwd, const char *model,
                            const char *effort)
{
    struct session *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->wake[0] = s->wake[1] = -1;
    pthread_mutex_init(&s->lock, NULL);
    s->backend = strdup(backend && *backend ? backend : "claude");
    if (!s->backend) {
        free(s);
        return NULL;
    }
    s->cwd = cwd ? strdup(cwd) : NULL;
    s->model = model ? strdup(model) : NULL;
    s->effort = effort ? strdup(effort) : NULL;
    s->thinking = 1;
    return s;
}

void session_replay(struct session *s)
{
    block_cleared();
    hud_print(s);
    if (!s)
        return;
    for (size_t i = 0; i < s->transcript.count; i++) {
        const struct transcript_turn *t = &s->transcript.turns[i];
        if (t->user && *t->user)
            prompt_echo_message(t->user);
        if (t->assistant && *t->assistant)
            md_render_kept(t->assistant, 0);
        if (t->interrupted) {
            ui_block_begin(0, 1);
            ui_error("  interrupted");
            ui_block_end();
        }
    }
    ui_flush();
}

void session_free(struct session *s)
{
    if (!s)
        return;
    livelist_forget(s);

    // A turn still running holds the agent this is about to close.
    if (s->running) {
        s->abort_request = 1;
        pthread_join(s->thread, NULL);
        s->running = 0;
        free(s->reply);
    }
    queue_drop(s);
    pthread_mutex_destroy(&s->lock);
    for (int i = 0; i < 2; i++)
        if (s->wake[i] >= 0)
            close(s->wake[i]);
    free(s->asked);
    if (s->agent)
        s->agent->close(s->agent);
    free(s->backend);
    free(s->cwd);
    free(s->workdir);
    free(s->model);
    free(s->effort);
    free(s->resolved);
    free(s->last_reply);
    free(s->failed_prompt);
    free(s->last_block);
    stream_reset(s);
    free(s->prompt);
    free(s->permission);
    free(s->error_note);
    free(s->system_extra);
    view_free(&s->view);
    transcript_free(&s->transcript);
    if (live == s)
        live = NULL;
    free(s);
}

static Backend *agent(struct session *s)
{
    if (s->agent)
        return s->agent;
    backend_opts o = {0};
    o.name = s->backend;
    o.cwd = s->cwd;
    o.model = s->model;
    o.effort = s->effort;
    o.allow_customizations = s->customizations;
    o.permission_mode = s->permission;
    o.fork_session = s->fork_session;

    const char *note = image_available()
        ? "This conversation is displayed in a terminal that renders images inline. "
          "To show the user an image, write a markdown image with an absolute local "
          "path — ![alt](/abs/path.png) — alone on its own line. PNG is drawn "
          "directly; other formats are converted first. Use this whenever an image "
          "would answer better than words: a render you just produced, a screenshot, "
          "a diagram, a photo the user asked about."
        : NULL;

    // A front end other than the terminal has its own conventions to teach the
    // agent, and they are appended to whatever this one already says.
    char *joined = NULL;
    if (note && s->system_extra) {
        size_t n = strlen(note) + strlen(s->system_extra) + 3;
        if ((joined = malloc(n)))
            snprintf(joined, n, "%s\n\n%s", note, s->system_extra);
    }
    o.system = joined ? joined : s->system_extra ? s->system_extra : note;
    s->agent = backend_open_ex(&o);
    free(joined);
    if (s->agent) {
        s->agent->set_event_cb(s->agent, on_event, s);
        s->agent->set_abort_check(s->agent, abort_check);
    }
    return s->agent;
}

static void await_model(struct session *s);
static void remember_window(const struct session *s);

int session_switch_backend(struct session *s, const char *backend)
{
    if (!s || !backend || !*backend)
        return 0;
    if (strcmp(s->backend, backend) == 0)
        return 1;

    char *handoff = transcript_handoff(&s->transcript, 128 * 1024);
    backend_opts o = {0};
    o.name = backend;
    o.system = handoff;
    o.cwd = s->cwd;
    o.allow_customizations = s->customizations;
    o.permission_mode = s->permission;
    Backend *replacement = backend_open_ex(&o);
    free(handoff);
    if (!replacement)
        return 0;

    if (!replacement->start(replacement, NULL)) {
        replacement->close(replacement);
        return 0;
    }
    replacement->set_event_cb(replacement, on_event, s);
    replacement->set_abort_check(replacement, abort_check);

    Backend *previous = s->agent;
    s->agent = replacement;
    replace(&s->backend, backend);
    replace(&s->model, NULL);
    replace(&s->effort, NULL);
    replace(&s->resolved, NULL);
    s->id[0] = '\0';
    const char *id = replacement->session_id(replacement);
    if (id) {

        snprintf(s->id, sizeof s->id, "%s", id);
        agenttabs_forget_hook(id);
    }
    s->turns = 0;
    s->cost_usd = 0;
    s->context_tokens = 0;
    s->context_window = 0;
    if (previous)
        previous->close(previous);

    await_model(s);
    return 1;
}

void session_set_quiet(struct session *s, int quiet) { s->quiet = quiet; }

void session_set_silent(struct session *s, int silent) { s->silent = silent; }

void session_set_observer(struct session *s, session_event_fn fn, void *ud)
{
    s->observer = fn;
    s->observer_ud = ud;
}

void session_set_system_extra(struct session *s, const char *text)
{
    replace(&s->system_extra, text);
}

void session_set_abort_hook(struct session *s, int (*fn)(void *ud), void *ud)
{
    s->abort_hook = fn;
    s->abort_ud = ud;
}

void session_set_naming(struct session *s, int on) { s->skip_naming = !on; }

void session_set_thinking(struct session *s, int on) { s->thinking = on; }

int session_thinking(const struct session *s) { return s->thinking; }

void session_set_compact(struct session *s, int on) { s->compact = on; }

int session_compact(const struct session *s) { return s->compact; }

void session_set_customizations(struct session *s, int on) { s->customizations = on; }

void session_set_fork(struct session *s, int on) { s->fork_session = on; }

static void set_id(struct session *s, const char *id)
{

    int changed = strcmp(s->id, id) != 0;
    snprintf(s->id, sizeof s->id, "%s", id);
    if (changed) {

        s->title[0] = '\0';
        s->stale_title[0] = '\0';
        s->announce_title = 0;
        s->named = 0;
        if (!s->retitle)
            title_lookup(s->id, s->title, sizeof s->title);
        status_set_note(s->title);
        publish(s, s->idle_busy ? "working" : "finished");
    }
    agenttabs_forget_hook(id);
}

static int restart(struct session *s, const char *resume_id)
{
    Backend *b = agent(s);
    if (!b || !b->start(b, resume_id))
        return 0;
    if (resume_id && resume_id != s->id)
        set_id(s, resume_id);

    const char *id = b->session_id(b);
    if (id)
        set_id(s, id);
    return 1;
}

int session_start(struct session *s)
{
    // The directory can be gone before the first turn — a session restored into
    // a worktree that was merged away. Start somewhere that exists instead.
    char next[4096];
    if (!dir_alive(s->cwd) && ground_target(s->cwd, next, sizeof next))
        replace(&s->cwd, next);
    if (!restart(s, s->id[0] ? s->id : NULL))
        return 0;
    publish(s, "finished");
    return 1;
}

int session_trust_project(struct session *s)
{
    Backend *b = agent(s);
    if (!b || !b->trust_project || !b->trust_project(b, s->cwd))
        return 0;
    return restart(s, s->id[0] ? s->id : NULL);
}

int session_take_trust_request(struct session *s)
{
    if (!s || !s->trust_requested)
        return 0;
    s->trust_requested = 0;
    return 1;
}

int session_set_model(struct session *s, const char *model)
{
    Backend *b = agent(s);
    if (!b)
        return 0;

    char *next = model ? strdup(model) : NULL;
    if (model && !next)
        return 0;
    char *previous = s->model;
    s->model = next;
    b->set_model(b, s->model);
    if (restart(s, s->id[0] ? s->id : NULL)) {
        free(previous);
        replace(&s->resolved, NULL);
        prefs_remember_choice("model", s->backend, s->model);
        await_model(s);
        remember_model(s);
        return 1;
    }
    free(s->model);
    s->model = previous;
    b->set_model(b, s->model);
    return 0;
}

int session_set_effort(struct session *s, const char *effort)
{
    Backend *b = agent(s);
    if (!b || !(b->caps & BACKEND_CAP_EFFORT) || !b->set_effort)
        return 0;

    char *next = effort ? strdup(effort) : NULL;
    if (effort && !next)
        return 0;
    char *previous = s->effort;
    s->effort = next;
    if (!b->set_effort(b, s->effort)) {
        free(s->effort);
        s->effort = previous;
        return 0;
    }
    if ((b->caps & BACKEND_CAP_LIVE_EFFORT) ||
        restart(s, s->id[0] ? s->id : NULL)) {
        free(previous);
        prefs_remember_choice("effort", s->backend, s->effort);
        return 1;
    }
    free(s->effort);
    s->effort = previous;
    b->set_effort(b, s->effort);
    return 0;
}

static const struct {
    const char *name;
    const char *desc;
} PERMISSIONS[] = {
    {"bypassPermissions", "never refuses a tool call"},
    {"auto", "approves the safe calls, refuses the rest"},
    {"acceptEdits", "edits without asking, refuses the rest"},
    {"dontAsk", "refuses anything that would ask"},
    {"manual", "refuses everything not pre-allowed"},
    {"plan", "read-only: research and propose, no changes"},
};
#define PERMISSION_COUNT (COUNT(PERMISSIONS))
#define PERMISSION_DEFAULT 1     /* auto */

int session_permission_count(void) { return PERMISSION_COUNT; }
int session_permission_default(void) { return PERMISSION_DEFAULT; }

const char *session_permission_name(int index)
{
    return (index >= 0 && index < PERMISSION_COUNT) ? PERMISSIONS[index].name : NULL;
}

const char *session_permission_desc(int index)
{
    return (index >= 0 && index < PERMISSION_COUNT) ? PERMISSIONS[index].desc : NULL;
}

int session_permission_index(const char *mode)
{
    if (!mode)
        return -1;
    for (int i = 0; i < PERMISSION_COUNT; i++)
        if (strcmp(PERMISSIONS[i].name, mode) == 0)
            return i;
    return -1;
}

const char *session_permission(const struct session *s)
{
    return (s && s->permission) ? s->permission : PERMISSIONS[PERMISSION_DEFAULT].name;
}

int session_set_permission(struct session *s, const char *mode)
{

    if (!s->agent) {
        replace(&s->permission, mode);
        return 1;
    }

    Backend *b = s->agent;
    char *previous = s->permission;
    s->permission = mode ? strdup(mode) : NULL;
    b->set_permission(b, s->permission);
    if (restart(s, s->id[0] ? s->id : NULL)) {
        free(previous);
        return 1;
    }
    free(s->permission);
    s->permission = previous;
    b->set_permission(b, s->permission);
    return 0;
}

void session_adopt_id(struct session *s, const char *id)
{
    if (s && id && *id)
        set_id(s, id);
}

int session_resume(struct session *s, const char *id)
{
    if (!restart(s, id))
        return 0;
    s->turns = 0;
    s->cost_usd = 0;
    s->context_tokens = 0;
    replace(&s->last_reply, NULL);
    replace(&s->failed_prompt, NULL);
    transcript_clear(&s->transcript);
    return 1;
}

int session_set_cwd(struct session *s, const char *path)
{
    if (!s || !path || !*path)
        return 0;
    if (s->cwd && strcmp(s->cwd, path) == 0)
        return 1;

    char *next = strdup(path);
    if (!next)
        return 0;

    Backend *previous = s->agent;
    char    *was = s->cwd;
    s->cwd = next;
    s->agent = NULL;
    if (!agent(s) || !s->agent->start(s->agent, NULL)) {
        if (s->agent)
            s->agent->close(s->agent);
        s->agent = previous;
        free(s->cwd);
        s->cwd = was;
        return 0;
    }
    if (previous)
        previous->close(previous);
    free(was);

    s->turns = 0;
    s->cost_usd = 0;
    s->context_tokens = 0;
    replace(&s->workdir, NULL);
    replace(&s->last_reply, NULL);
    replace(&s->failed_prompt, NULL);
    replace(&s->last_block, NULL);
    transcript_clear(&s->transcript);
    s->title[0] = '\0';
    s->stale_title[0] = '\0';
    s->announce_title = 0;
    s->retitle = 1;
    s->named = 0;
    status_set_note(NULL);

    s->id[0] = '\0';
    const char *id = s->agent->session_id(s->agent);
    if (id)
        set_id(s, id);
    gitinfo_forget();
    await_model(s);
    return 1;
}

int session_clear(struct session *s)
{
    if (!s->agent || !s->agent->reset(s->agent))
        return 0;
    s->turns = 0;
    s->cost_usd = 0;
    s->context_tokens = 0;
    replace(&s->last_reply, NULL);
    replace(&s->failed_prompt, NULL);
    replace(&s->last_block, NULL);
    transcript_clear(&s->transcript);

    s->title[0] = '\0';
    s->stale_title[0] = '\0';
    s->announce_title = 0;
    s->retitle = 1;
    s->named = 0;
    status_set_note(NULL);

    const char *id = s->agent->session_id(s->agent);
    if (id)
        set_id(s, id);
    else
        s->id[0] = '\0';
    return 1;
}

static void update_title(struct session *s)
{
    if (!s->id[0] || s->title[0])
        return;
    if (!s->named) {
        name_poll(s);
        return;
    }
    adopt_title(s);
}

static void print_footer(struct session *s, double elapsed)
{
    char used[32], window[32];
    humanize(s->context_tokens, used, sizeof used);
    humanize(s->context_window, window, sizeof window);

    char line[384];
    size_t n = 0;
    #define APPEND(...)                                                                        \
        do {                                                                                   \
            int w = snprintf(line + n, sizeof line - n, __VA_ARGS__);                           \
            if (w > 0)                                                                          \
                n += (size_t)w < sizeof line - n ? (size_t)w : sizeof line - n - 1;             \
        } while (0)

    APPEND("%.0fs", elapsed);
    if (s->context_window > 0) {
        int percent = (int)((double)s->context_tokens * 100.0 / (double)s->context_window);
        APPEND(" · %s / %s (%d%%)", used, window, percent);
    } else if (s->context_tokens > 0) {
        APPEND(" · %s", used);
    }
    if (s->cost_usd > 0)
        APPEND(" · $%.4f", s->cost_usd);

    int wrapped = 0;
    if (s->title[0]) {
        int room = ui_columns() - 1;
        size_t want = ui_cells(line) + ui_cells(" · ") + ui_cells(s->title);
        if (room > 0 && want <= (size_t)room)
            APPEND(" · %s", s->title);
        else
            wrapped = 1;
    }
    #undef APPEND

    ui_esc(ui_style(UI_DIM));
    ui_put(line);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
    if (wrapped)
        ui_wrapped(s->title, 0, UI_DIM);
    ui_flush();
}

// A session's directory can vanish underneath it — a worktree that was merged
// and then cleaned up. The child is stranded: with no working directory it
// cannot spawn a shell, so hooks and tools fail with errors that name /bin/sh
// rather than the folder that went missing. Only a restart somewhere that
// exists frees it.
static int dir_alive(const char *path)
{
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int nearest_live_dir(const char *path, char *out, size_t size)
{
    snprintf(out, size, "%s", path);
    for (char *slash; (slash = strrchr(out, '/')); ) {
        if (slash == out)
            out[1] = '\0';
        else
            *slash = '\0';
        if (dir_alive(out))
            return 1;
        if (slash == out)
            return 0;
    }
    return 0;
}

// The repository a deleted worktree belonged to, which beats landing in
// whatever container directory happens to survive above it.
static int main_worktree(const char *near, char *out, size_t size)
{
    char quoted[4200];
    if (!text_shell_quote(near, quoted, sizeof quoted))
        return 0;

    char cmd[4300];
    if (snprintf(cmd, sizeof cmd,
                 "git -C %s rev-parse --path-format=absolute --git-common-dir 2>/dev/null",
                 quoted) >= (int)sizeof cmd)
        return 0;

    FILE *f = popen(cmd, "r");
    if (!f)
        return 0;
    char line[4096] = "";
    char *got = fgets(line, sizeof line, f);
    pclose(f);
    if (!got)
        return 0;
    line[strcspn(line, "\n")] = '\0';

    char *slash = strrchr(line, '/');       // .../<repo>/.git -> .../<repo>
    if (!slash || slash == line || strcmp(slash + 1, ".git") != 0)
        return 0;
    *slash = '\0';
    if (!dir_alive(line))
        return 0;
    snprintf(out, size, "%s", line);
    return 1;
}

// Where a session whose directory went missing should carry on: the repository
// the deleted worktree belonged to, else the nearest surviving ancestor.
static int ground_target(const char *gone, char *out, size_t size)
{
    char near[4096];
    if (!nearest_live_dir(gone, near, sizeof near))
        return 0;
    if (main_worktree(near, out, size))
        return 1;
    if (!strcmp(near, "/")) {
        const char *home = getenv("HOME");
        if (dir_alive(home)) {
            snprintf(out, size, "%s", home);
            return 1;
        }
    }
    snprintf(out, size, "%s", near);
    return 1;
}

__attribute__((format(printf, 2, 3)))
static void session_warn(struct session *s, const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);

    backend_event ev = {.kind = BACKEND_EV_WARNING, .text = text};
    struct session *was = session_set_drawing(s);
    on_event(s, &ev);
    session_set_drawing(was);
}

static void shorten(const char *dir, char *out, size_t size)
{
    path_home_relative(dir, out, size);
    if (!*out)
        snprintf(out, size, "%s", dir ? dir : "?");
}

// GROUND_OK: nothing to do. GROUND_MOVED: the child was restarted somewhere
// that exists. GROUND_LOST: there was nowhere left to go.
enum { GROUND_LOST, GROUND_OK, GROUND_MOVED };

static int session_reground(struct session *s)
{
    const char *dir = s->workdir ? s->workdir : s->cwd;
    if (dir_alive(dir))
        return GROUND_OK;

    char gone[4096], shown[512];
    snprintf(gone, sizeof gone, "%s", dir ? dir : "");
    shorten(gone, shown, sizeof shown);

    // Only the worktree the agent had moved into is gone: the session's own
    // directory still holds its history, so this one resumes.
    if (dir_alive(s->cwd)) {
        replace(&s->workdir, NULL);
        if (restart(s, s->id[0] ? s->id : NULL)) {
            char home[512];
            shorten(s->cwd, home, sizeof home);
            session_warn(s, "%s is gone — restarted in %s", shown, home);
            return GROUND_MOVED;
        }
    } else {
        char next[4096], moved[512];
        if (ground_target(gone, next, sizeof next)) {
            shorten(next, moved, sizeof moved);
            if (session_set_cwd(s, next)) {
                session_warn(s, "%s is gone — restarted in %s with a fresh context",
                             shown, moved);
                return GROUND_MOVED;
            }
        }
    }

    char note[700];
    snprintf(note, sizeof note,
             "the working directory %s no longer exists and the session could not "
             "be restarted elsewhere", shown);
    replace(&s->error_note, note);
    session_warn(s, "%s", note);
    return GROUND_LOST;
}

// No turn can start with the directory gone: the child would be stranded there.
static int turn_ready(struct session *s, const char *text)
{
    replace(&s->error_note, NULL);
    if (session_reground(s))
        return 1;
    replace(&s->failed_prompt, text);
    return 0;
}

static void turn_prepare(struct session *s, const char *text)
{
    replace(&s->last_block, NULL);
    stream_reset(s);
    replace(&s->prompt, text);
    s->view.after_activity = 0;
    s->view.after_tool = 0;
    s->view.after_collapse = 0;
    view_cluster_forget(&s->view);
    s->started = now_seconds();
    s->idle_busy = 1;
    s->interrupted = 0;
    s->abort_request = 0;
    publish(s, "working");
}

// Everything a turn leaves behind, once its stream has been drawn: the reply,
// the accounting, the transcript entry, the footer. Takes `reply`.
static int turn_finish(struct session *s, char *reply, const backend_result *meta,
                       double elapsed)
{
    const backend_result m = *meta;
    const char *text = s->prompt ? s->prompt : "";
    const char *id = s->agent->session_id(s->agent);
    if (id)
        set_id(s, id);

    if (!reply) {
        replace(&s->failed_prompt, text);
        s->idle_busy = 0;
        publish(s, "errored");
        // The directory can go away mid-turn; then the stderr tail is some
        // downstream complaint about /bin/sh, not the reason.
        if (session_reground(s) == GROUND_MOVED)
            replace(&s->error_note,
                    "the working directory was deleted mid-turn; the session has "
                    "been restarted");
        const char *detail = session_last_error(s);
        if (s->silent)
            return 0;
        // Quiet is a caller reading stdout for the answer: a reason written
        // there would be taken for one.
        if (s->quiet) {
            if (detail)
                fprintf(stderr, "%s: %s\n", s->backend, detail);
            else
                fprintf(stderr, "the %s process stopped responding\n", s->backend);
            return 0;
        }
        ui_block_begin(1, 1);
        if (detail)
            ui_error("%s: %s", s->backend, detail);
        else
            ui_error("the %s process stopped responding", s->backend);
        ui_block_end();
        return 0;
    }

    int shown = (s->last_block && strcmp(reply, s->last_block) == 0) ||
                (s->streamed && strcmp(reply, s->streamed) == 0);
    if (s->silent) {
        // Nothing is drawn here: the front end that asked for the turn takes
        // the answer from session_last_reply() and says it its own way.
        if (!*reply && s->last_block)
            replace(&reply, s->last_block);
    } else if (s->quiet) {
        // A turn can end with no closing text — the last thing it said is the
        // answer then, and if it never said anything the caller still needs a
        // reason, which only stderr can carry once stdout is empty.
        const char *tail = *reply ? reply : (s->last_block ? s->last_block : "");
        if (*tail) {
            ui_put(tail);
            ui_put("\n");
        } else {
            const char *detail = s->agent->last_error(s->agent);
            char why[128];
            // The driver's own word for how the turn ended, when it has one and
            // it is not the ordinary one: an empty answer is never expected, so
            // whatever the backend can say about it is worth carrying out.
            if (m.subtype[0] && strcmp(m.subtype, "success") != 0)
                snprintf(why, sizeof why, "the turn ended without a reply (%s)",
                         m.subtype);
            else
                snprintf(why, sizeof why, "the turn ended without a reply");
            fprintf(stderr, "%s\n",
                    detail && *detail ? detail
                    : m.interrupted   ? "the turn was interrupted"
                    : m.is_error      ? "the turn ended in an error"
                                      : why);
        }
    } else if (*reply && !shown) {
        md_render_kept(reply, 0);
    }
    s->interrupted = m.interrupted;
    if (m.interrupted && !s->silent) {
        ui_block_begin(0, 1);
        ui_error("  interrupted");
        ui_block_end();
    }

    if (*reply)
        replace(&s->last_reply, reply);
    if (m.is_error) {
        replace(&s->failed_prompt, text);
    } else {
        transcript_add(&s->transcript, s->backend, text, reply, m.interrupted);
        replace(&s->failed_prompt, NULL);
    }
    free(reply);

    s->turns++;
    if (m.cost_usd > 0)
        s->cost_usd = m.cost_usd;

    if (m.context_window > 0)
        s->context_window = m.context_window;
    if (m.context_tokens > 0)
        s->context_tokens = m.context_tokens;

    tab_busy(s, session_idle_busy(s));

    gitinfo_forget();

    update_title(s);
    remember_model(s);
    remember_window(s);
    if (!s->quiet && !s->silent)
        print_footer(s, elapsed);
    return 1;
}

int session_turn(struct session *s, const char *text)
{
    if (!s->agent || s->running || !turn_ready(s, text))
        return 0;

    turn_prepare(s, text);
    struct session *was = session_set_drawing(s);
    if (!s->quiet && !s->silent) {
        set_spin_word(s);
        status_begin();
    }

    backend_result meta = {0};
    char *reply = s->agent->ask_ex(s->agent, text, &meta);
    quota_poll(s);
    double elapsed = now_seconds() - s->started;
    session_set_drawing(was);
    if (!s->quiet && !s->silent)
        status_end();

    return turn_finish(s, reply, &meta, elapsed);
}

static void wake_write(struct session *s)
{
    if (s->wake[1] < 0)
        return;
    char byte = 1;
    ssize_t w = write(s->wake[1], &byte, 1);
    (void)w;
}

static void wake_drain(struct session *s)
{
    if (s->wake[0] < 0)
        return;
    char buf[256];
    while (read(s->wake[0], buf, sizeof buf) > 0)
        ;
}

static int wake_open(struct session *s)
{
    if (s->wake[0] >= 0)
        return 1;
    if (pipe(s->wake) != 0)
        return 0;
    for (int i = 0; i < 2; i++) {
        fcntl(s->wake[i], F_SETFL, fcntl(s->wake[i], F_GETFL, 0) | O_NONBLOCK);
        fcntl(s->wake[i], F_SETFD, FD_CLOEXEC);
    }
    return 1;
}

// The turn itself. It draws nothing and touches nothing the screen owns: the
// events go to the queue, and the reply waits here to be collected.
static void *turn_thread(void *ud)
{
    struct session *s = ud;
    owner = s;
    // The restart signal is the main thread's to notice.
    restart_shield_thread();

    memset(&s->meta, 0, sizeof s->meta);
    s->reply = s->agent->ask_ex(s->agent, s->asked, &s->meta);
    s->finished = 1;
    wake_write(s);
    return NULL;
}

int session_turn_begin(struct session *s, const char *text)
{
    if (!s || !s->agent || s->running || !wake_open(s) || !turn_ready(s, text))
        return 0;

    turn_prepare(s, text);
    replace(&s->asked, text);
    s->reply = NULL;
    s->finished = 0;
    s->running = 1;

    if (pthread_create(&s->thread, NULL, turn_thread, s) != 0) {
        s->running = 0;
        return 0;
    }
    return 1;
}

int session_turn_running(const struct session *s)
{
    return s && s->running;
}

double session_turn_elapsed(const struct session *s)
{
    return s && s->running ? now_seconds() - s->started : 0;
}

void session_interrupt(struct session *s)
{
    if (s && s->running)
        s->abort_request = 1;
}

// A turn that ended where nobody was looking. Set by the window, which is the
// only thing that knows which of its sessions was in front at the time, and
// republished so the other windows' lists say the same.
void session_set_unseen(struct session *s, int on)
{
    on = on ? 1 : 0;
    if (!s || s->unseen == on)
        return;
    s->unseen = on;
    publish(s, session_busy(s) ? "working" : "finished");
}

int session_unseen(const struct session *s) { return s ? s->unseen : 0; }

int session_busy(const struct session *s)
{
    if (!s)
        return 0;
    return s->running || session_idle_busy(s);
}

static void drain_events(struct session *s)
{
    struct evcopy *e;
    struct session *was = session_set_drawing(s);
    while ((e = dequeue(s))) {
        render_event(s, &e->ev);
        evcopy_free(e);
    }
    session_set_drawing(was);
}

int session_turn_pump(struct session *s)
{
    if (!s || !s->running)
        return 0;

    wake_drain(s);
    drain_events(s);
    name_poll(s);
    quota_poll(s);

    if (!s->finished)
        return 1;

    pthread_join(s->thread, NULL);
    s->running = 0;
    // Anything the turn queued between the last drain and its own end.
    drain_events(s);

    char *reply = s->reply;
    s->reply = NULL;
    turn_finish(s, reply, &s->meta, now_seconds() - s->started);
    return 0;
}

void session_turn_wait(struct session *s)
{
    while (s && s->running) {
        if (!session_turn_pump(s))
            break;
        struct timespec ts = {0, 20 * 1000000L};
        nanosleep(&ts, NULL);
    }
}

const char *session_title(const struct session *s)
{
    return s && s->title[0] ? s->title : NULL;
}

const char *session_model(const struct session *s)
{
    if (s->resolved && *s->resolved)
        return s->resolved;
    if (s->model && *s->model)
        return s->model;
    return "default";
}

const char *session_effort(const struct session *s)
{
    return s->effort && *s->effort ? s->effort : "default";
}

const char *session_saved_model(const char *backend)
{
    return prefs_saved_choice("model", backend);
}

const char *session_saved_effort(const char *backend)
{
    return prefs_saved_choice("effort", backend);
}

static const char *backend_model(const struct session *s)
{
    if (!s->agent || !s->agent->model)
        return NULL;
    const char *got = s->agent->model(s->agent);
    return got && *got ? got : NULL;
}

static void remember_model(const struct session *s)
{
    const char *id = s->resolved && *s->resolved ? s->resolved : backend_model(s);
    prefs_remember_resolved_model(s->backend, s->model, id);
}

static void await_model(struct session *s)
{
    if (!s->agent || !s->agent->model)
        return;
    double deadline = now_seconds() + 1.5;
    while (!backend_model(s) && now_seconds() < deadline) {
        struct timespec nap = {0, 20 * 1000 * 1000};
        nanosleep(&nap, NULL);
    }
}

const char *session_model_short(const struct session *s, const char *model)
{
    if (strcmp(s->backend, "claude") == 0 && strncmp(model, "claude-", 7) == 0 && model[7])
        return model + 7;
    return model;
}

const char *session_model_label(const struct session *s)
{
    if (s->resolved && *s->resolved)
        return s->resolved;

    if (s->agent && s->agent->model) {
        const char *got = s->agent->model(s->agent);
        if (got && *got)
            return got;
    }
    const char *cached = prefs_resolved_model(s->backend, s->model);
    if (cached)
        return cached;
    if (s->model && *s->model)
        return s->model;
    return "default";
}

static void remember_window(const struct session *s)
{
    prefs_remember_window(s->backend, session_model_label(s), s->context_window);
}

long session_context_window(const struct session *s)
{
    if (s->context_window > 0)
        return s->context_window;
    if (s->agent && s->agent->usage) {
        long used = 0, window = 0;
        s->agent->usage(s->agent, &used, &window);
        if (window > 0)
            return window;
    }
    return prefs_window(s->backend, session_model_label(s));
}

const char *session_effort_label(const struct session *s)
{
    if (!session_can_set_effort(s))
        return NULL;
    const char *effort = spin_effort(s);
    return effort_is_off(effort) || !strcmp(effort, "default") ? NULL : effort;
}

int session_can_set_effort(const struct session *s)
{
    return s->agent && (s->agent->caps & BACKEND_CAP_EFFORT);
}

const char *session_id(const struct session *s) { return s->id[0] ? s->id : NULL; }

int session_can_resume(const struct session *s)
{
    return s->agent && (s->agent->caps & BACKEND_CAP_RESUME);
}

const char *session_cwd(const struct session *s) { return s->cwd; }
const char *session_workdir(const struct session *s)
{
    return s->workdir ? s->workdir : s->cwd;
}
const char *session_backend(const struct session *s) { return s->backend; }

static int argv_pair(char **out, int n, int max, const char *flag, const char *value)
{
    if (n + 2 > max)
        return n;
    out[n++] = (char *)flag;
    out[n++] = (char *)value;
    return n;
}

// The argv that opens this session again, for /restart, /fork and the note on
// the way out. One builder: the four of them used to disagree.
int session_argv(const struct session *s, char **out, int max, unsigned what)
{
    int n = argv_pair(out, 0, max, "-b", session_backend(s));

    const char *cwd = session_cwd(s);
    if ((what & SESSION_ARGV_CWD) && cwd && *cwd)
        n = argv_pair(out, n, max, "-C", cwd);

    const char *model = session_model(s);
    if (strcmp(model, "default"))
        n = argv_pair(out, n, max, "-m", model);

    const char *effort = session_effort(s);
    if (strcmp(effort, "default"))
        n = argv_pair(out, n, max, "-e", effort);

    if ((what & SESSION_ARGV_SAFE) && !s->customizations && n < max)
        out[n++] = (char *)"-s";

    const char *id = session_id(s);
    if ((what & SESSION_ARGV_RESUME) && id && *id && session_can_resume(s))
        n = argv_pair(out, n, max, "--session", id);

    return n;
}

int session_last_interrupted(const struct session *s) { return s ? s->interrupted : 0; }

const char *session_last_error(const struct session *s)
{
    if (s && s->error_note)
        return s->error_note;
    return s && s->agent ? s->agent->last_error(s->agent) : NULL;
}
const char *session_last_reply(const struct session *s) { return s->last_reply; }
const char *session_failed_prompt(const struct session *s)
{
    return s ? s->failed_prompt : NULL;
}

int session_context_percent(const struct session *s)
{
    long used = s->context_tokens, window = s->context_window;

    if (s->agent && s->agent->usage) {
        long live_used = 0, live_window = 0;
        s->agent->usage(s->agent, &live_used, &live_window);
        if (live_used > 0)
            used = live_used;
        if (live_window > 0)
            window = live_window;
    }
    if (used <= 0 || window <= 0)
        return -1;
    int percent = (int)(used * 100 / window);
    return percent > 100 ? 100 : percent;
}

static const char *auth_description(const struct session *s)
{
    const char *source = s->agent ? s->agent->auth_source(s->agent) : NULL;
    if (!source)
        return NULL;
    if (strcmp(source, "none") == 0)
        return "subscription login";
    return source;
}

void session_spin_word(const struct session *s)
{
    if (s)
        set_spin_word(s);
}

void session_report(const struct session *s)
{
    char used[32], window[32];
    humanize(s->context_tokens, used, sizeof used);
    humanize(s->context_window, window, sizeof window);

    const char *auth = auth_description(s);
    ui_block_begin(1, 1);
    ui_note("  backend  %s", s->backend);
    ui_note("  model    %s", session_model(s));
    if (session_can_set_effort(s))
        ui_note("  effort   %s", session_effort(s));
    if (auth)
        ui_note("  auth     %s", auth);

    if (strcmp(s->backend, "claude") == 0) {
        ui_note("  config   %s", s->customizations ? "skills, CLAUDE.md, MCP, agents"
                                                   : "safe mode (customizations off)");
        ui_note("  tools    %s", session_permission(s));
    }
    ui_note("  calls    %s", s->compact ? "compact (one row each)" : "full blocks");
    if (tg_label())
        ui_note("  chat     %s", tg_label());
    if (s->id[0])
        ui_note("  session  %s", s->id);

    char scratch[512];
    path_home_relative(s->cwd, scratch, sizeof scratch);
    const char *dir = s->cwd ? scratch : ".";
    int room = ui_columns() - 12;
    size_t cells = ui_cells(dir);
    if (room > 8 && cells > (size_t)room) {

        while (*dir && ui_cells(dir) > (size_t)room - 1)
            dir++;
        ui_note("  cwd      …%s", dir);
    } else {
        ui_note("  cwd      %s", dir);
    }
    ui_note("  turns    %d", s->turns);
    if (s->context_window > 0)
        ui_note("  context  %s / %s", used, window);
    if (s->cost_usd > 0 || auth)
        ui_note("  cost     $%.4f%s", s->cost_usd,
                auth && strcmp(auth, "subscription login") == 0
                    ? "  (list price; the subscription is not billed per token)"
                    : "");
    ui_block_end();
}
