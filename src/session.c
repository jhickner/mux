#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "agenttabs.h"
#include "app.h"
#include "banner.h"
#include "prompt.h"
#include "filediff.h"
#include "gitinfo.h"
#include "image.h"
#include "md.h"
#include "sessionprefs.h"
#include "sessionview.h"
#include "settings.h"
#include "status.h"
#include "title.h"
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
    char    *model;
    char    *effort;
    char    *resolved;
    char     id[128];
    char     title[128];
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
    int      skip_naming;
    char     footer[384];
    int      hold_footer;
    int      thinking;
    int      compact;
    int      customizations;
    char    *permission;
    int      idle_busy;

    struct turnview view;
};

static void replace(char **slot, const char *value)
{
    free(*slot);
    *slot = value ? strdup(value) : NULL;
}

/* Doubling rather than an exact realloc per event: this runs once per streamed
   assistant chunk, so an exact fit would make a long reply quadratic. */
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

static const char *shorten_path(const struct session *s, const char *value, char *scratch,
                                size_t size)
{
    if (value[0] != '/')
        return value;
    if (s->cwd) {
        size_t n = strlen(s->cwd);
        if (strncmp(value, s->cwd, n) == 0 && value[n] == '/')
            return value + n + 1;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        size_t n = strlen(home);
        if (strncmp(value, home, n) == 0 && value[n] == '/') {
            snprintf(scratch, size, "~/%s", value + n + 1);
            return scratch;
        }
    }
    return value;
}

/* One list for both readers: tool_argument takes the first match in this order,
   tool_path considers only the entries flagged as naming a file. */
static const struct {
    const char *key;
    int         is_path;
} TOOL_ARG_KEYS[] = {
    {"command", 0},          {"file_path", 1}, {"target_file", 1},
    {"path", 1},             {"target_directory", 0},
    {"pattern", 0},          {"url", 0},       {"query", 0},
    {"prompt", 0},           {"description", 0},
    {"notebook_path", 1},
};

static void tool_argument(const struct session *s, const backend_event *ev, char *out, size_t size)
{
    char arg[4096] = "";

    if (ev->input_json) {
        cJSON *input = cJSON_Parse(ev->input_json);
        if (input) {
            for (size_t i = 0; i < COUNT(TOOL_ARG_KEYS); i++) {
                const char *v =
                    cJSON_GetStringValue(cJSON_GetObjectItem(input, TOOL_ARG_KEYS[i].key));
                if (v && *v) {
                    char scratch[1024];
                    text_one_line(shorten_path(s, v, scratch, sizeof scratch), arg, sizeof arg);
                    break;
                }
            }
            cJSON_Delete(input);
        }
    } else if (ev->arg) {
        text_one_line(ev->arg, arg, sizeof arg);
    }
    snprintf(out, size, "%s", arg);
}

static int tool_path(const struct session *s, const char *input_json, char *out, size_t size)
{
    if (!input_json)
        return 0;

    cJSON *input = cJSON_Parse(input_json);
    if (!input)
        return 0;

    const char *found = NULL;
    for (size_t i = 0; i < COUNT(TOOL_ARG_KEYS) && !found; i++) {
        if (!TOOL_ARG_KEYS[i].is_path)
            continue;
        const char *v =
            cJSON_GetStringValue(cJSON_GetObjectItem(input, TOOL_ARG_KEYS[i].key));
        if (v && *v)
            found = v;
    }
    if (found) {
        if (found[0] == '/')
            snprintf(out, size, "%s", found);
        else
            snprintf(out, size, "%s/%s", s->cwd ? s->cwd : ".", found);
    }
    cJSON_Delete(input);
    return found != NULL;
}

static struct session *live;

static void remember_model(const struct session *s);
static void await_model(struct session *s);

static void on_event(void *ud, const backend_event *ev)
{
    struct session *s = ud;

    if (ev->kind == BACKEND_EV_INIT) {
        replace(&s->resolved, ev->name);
        return;
    }

    if (s->quiet || !live)
        return;

    switch (ev->kind) {
    case BACKEND_EV_INIT: /* handled above; listed to keep -Wswitch exhaustive */
        break;

    case BACKEND_EV_ASSISTANT:
        if (!ev->text || !*ev->text)
            break;
        status_pause();

        if (s->view.after_activity)
            ui_put("\n");
        md_render(ev->text, 0);
        ui_put("\n");
        status_resume();
        replace(&s->last_block, ev->text);
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
        view_cluster_forget(&s->view);
        if (s->view.after_tool)
            ui_put("\n");
        view_activity("\xe2\x9c\xbb", ev->text, UI_THINKING);
        status_resume();
        s->view.after_activity = 1;
        s->view.after_tool = 1;
        s->view.after_collapse = 0;
        break;

    case BACKEND_EV_TOOL: {
        const char *name = ev->name ? ev->name : "?";
        char arg[4096];
        tool_argument(s, ev, arg, sizeof arg);
        int collapsed = s->compact || toolstyle_collapses(name, ev->input_json, ev->arg);

        status_pause();
        if (collapsed) {
            if (!view_cluster_extend(&s->view, name, arg)) {
                if (s->view.after_tool && !s->view.after_collapse)
                    ui_put("\n");
                view_cluster_start(&s->view, name, arg);
            }
            view_cluster_paint(&s->view);
        } else {
            view_cluster_forget(&s->view);
            if (s->view.after_tool)
                ui_put("\n");
            view_tool_call(name, arg);
        }
        status_resume();

        char path[4096];
        if (!collapsed && tool_path(s, ev->input_json, path, sizeof path))
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
            view_cluster_forget(&s->view);
            filediff_clear();
            {
                const char *why = ev->text && *ev->text ? ev->text : NULL;
                if (!why || !strcmp(why, "failed")) {
                    view_tool_output("failed", UI_ERROR);
                } else {
                    char line[4096];
                    snprintf(line, sizeof line, "failed: %s", why);
                    view_tool_output(line, UI_ERROR);
                }
            }
            status_resume();
            s->view.after_activity = 1;
            s->view.after_tool = 1;
            s->view.after_collapse = 0;
            break;
        }

        if (!s->view.after_collapse) {
            status_pause();

            int drew;
            if (ev->diff) {
                drew = filediff_render_patch(ev->diff);
                filediff_clear();
            } else {
                drew = filediff_render();
            }
            if (!drew)
                view_tool_output(ev->text, UI_DIM);
            status_resume();
        }
        s->view.after_activity = 1;
        s->view.after_tool = 1;
        break;
    }

    status_gap(s->view.after_tool);
    ui_flush();
}

int session_idle_fd(const struct session *s)
{
    if (!s || !s->agent || !s->agent->idle_fd || s->quiet)
        return -1;
    return s->agent->idle_fd(s->agent);
}

/* Only report the change: the tab record is a file the tmux status bar reads. */
static void tab_busy(struct session *s, int busy)
{
    busy = busy ? 1 : 0;
    if (busy == s->idle_busy)
        return;
    s->idle_busy = busy;
    if (busy)
        agenttabs_working();
    else
        agenttabs_finished();
}

int session_idle_pump(struct session *s)
{
    if (!s || !s->agent || !s->agent->idle_pump || s->quiet)
        return 0;

    view_cluster_forget(&s->view);
    s->view.after_activity = 0;
    s->view.after_tool = 0;
    s->view.after_collapse = 0;
    live = s;
    int busy = s->agent->idle_pump(s->agent) ? 1 : 0;
    live = NULL;

    tab_busy(s, busy);
    gitinfo_forget(); /* a background worker's turn edits the tree too */
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

    if (s->skip_naming)
        return;

    if (!s->named) {
        if (!s->prompt)
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
    if (title_lookup(s->id, s->title, sizeof s->title))
        status_set_note(s->title);
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

static int abort_check(void)
{
    name_poll(live);
    quota_poll(live);
    if (live)
        set_spin_word(live);

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

    status_tick();
    return interrupt;
}

struct session *session_new(const char *backend, const char *cwd, const char *model,
                            const char *effort)
{
    struct session *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
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
    ui_esc(UI_CLEAR_SCREEN);
    banner_hints();
    if (!s)
        return;
    for (size_t i = 0; i < s->transcript.count; i++) {
        const struct transcript_turn *t = &s->transcript.turns[i];
        if (t->user && *t->user)
            prompt_echo_message(t->user);
        if (t->assistant && *t->assistant) {
            md_render(t->assistant, 0);
            ui_put("\n");
        }
        if (t->interrupted) {
            ui_error("  interrupted");
            ui_put("\n");
        }
    }
    ui_flush();
}

void session_free(struct session *s)
{
    if (!s)
        return;
    if (s->agent)
        s->agent->close(s->agent);
    free(s->backend);
    free(s->cwd);
    free(s->model);
    free(s->effort);
    free(s->resolved);
    free(s->last_reply);
    free(s->failed_prompt);
    free(s->last_block);
    stream_reset(s);
    free(s->prompt);
    free(s->permission);
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

    if (image_available())
        o.system =
            "This conversation is displayed in a terminal that renders images inline. "
            "To show the user an image, write a markdown image with an absolute local "
            "path — ![alt](/abs/path.png) — alone on its own line. PNG is drawn "
            "directly; other formats are converted first. Use this whenever an image "
            "would answer better than words: a render you just produced, a screenshot, "
            "a diagram, a photo the user asked about.";
    s->agent = backend_open_ex(&o);
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
    s->footer[0] = '\0';
    s->context_window = 0;
    if (previous)
        previous->close(previous);

    await_model(s);
    return 1;
}

void session_set_quiet(struct session *s, int quiet) { s->quiet = quiet; }

void session_set_naming(struct session *s, int on) { s->skip_naming = !on; }

void session_hold_footer(struct session *s, int on) { s->hold_footer = on; }

const char *session_footer(const struct session *s)
{
    return s->footer[0] ? s->footer : NULL;
}

void session_set_thinking(struct session *s, int on) { s->thinking = on; }

int session_thinking(const struct session *s) { return s->thinking; }

void session_set_compact(struct session *s, int on) { s->compact = on; }

int session_compact(const struct session *s) { return s->compact; }

void session_set_customizations(struct session *s, int on) { s->customizations = on; }

static void set_id(struct session *s, const char *id)
{

    int changed = strcmp(s->id, id) != 0;
    snprintf(s->id, sizeof s->id, "%s", id);
    if (changed) {

        s->title[0] = '\0';
        s->named = 0;
        if (!s->retitle)
            title_lookup(s->id, s->title, sizeof s->title);
        status_set_note(s->title);
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

int session_start(struct session *s) { return restart(s, s->id[0] ? s->id : NULL); }

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

int session_permission_count(void) { return PERMISSION_COUNT; }

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
    return (s && s->permission) ? s->permission : PERMISSIONS[0].name;
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
    s->footer[0] = '\0';
    replace(&s->last_reply, NULL);
    replace(&s->failed_prompt, NULL);
    transcript_clear(&s->transcript);
    return 1;
}

int session_clear(struct session *s)
{
    if (!s->agent || !s->agent->reset(s->agent))
        return 0;
    s->turns = 0;
    s->cost_usd = 0;
    s->context_tokens = 0;
    s->footer[0] = '\0';
    replace(&s->last_reply, NULL);
    replace(&s->failed_prompt, NULL);
    replace(&s->last_block, NULL);
    transcript_clear(&s->transcript);

    s->title[0] = '\0';
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
    if (title_lookup(s->id, s->title, sizeof s->title))
        status_set_note(s->title);
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

    snprintf(s->footer, sizeof s->footer, "%s%s%s", line,
             wrapped ? " · " : "", wrapped ? s->title : "");
    if (s->hold_footer)
        return;

    ui_esc(ui_style(UI_DIM));
    ui_put(line);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
    if (wrapped)
        ui_wrapped(s->title, 0, UI_DIM);
    ui_put("\n");
    ui_flush();
}

int session_turn(struct session *s, const char *text)
{
    if (!s->agent)
        return 0;

    replace(&s->last_block, NULL);
    stream_reset(s);
    replace(&s->prompt, text);
    s->view.after_activity = 0;
    s->view.after_tool = 0;
    s->view.after_collapse = 0;
    view_cluster_forget(&s->view);
    live = s;
    double started = now_seconds();
    s->idle_busy = 1;
    agenttabs_working();
    if (!s->quiet) {
        set_spin_word(s);
        status_begin();
    }

    backend_result meta = {0};
    char *reply = s->agent->ask_ex(s->agent, text, &meta);
    quota_poll(s);
    double elapsed = now_seconds() - started;
    live = NULL;
    if (!s->quiet)
        status_end();

    const char *id = s->agent->session_id(s->agent);
    if (id)
        set_id(s, id);

    if (!reply) {
        replace(&s->failed_prompt, text);
        s->idle_busy = 0;
        agenttabs_errored();
        const char *detail = s->agent->last_error(s->agent);
        if (detail)
            ui_error("%s: %s", s->backend, detail);
        else
            ui_error("the %s process stopped responding", s->backend);
        ui_put("\n");
        return 0;
    }

    int shown = (s->last_block && strcmp(reply, s->last_block) == 0) ||
                (s->streamed && strcmp(reply, s->streamed) == 0);
    if (s->quiet) {
        if (*reply) {
            ui_put(reply);
            ui_put("\n");
        }
    } else if (*reply && !shown) {
        if (s->view.after_activity)
            ui_put("\n");
        md_render(reply, 0);
        ui_put("\n");
    }
    if (meta.interrupted) {
        ui_error("  interrupted");
        ui_put("\n");
    }

    if (*reply)
        replace(&s->last_reply, reply);
    if (meta.is_error) {
        replace(&s->failed_prompt, text);
    } else {
        transcript_add(&s->transcript, s->backend, text, reply, meta.interrupted);
        replace(&s->failed_prompt, NULL);
    }
    free(reply);

    s->turns++;
    if (meta.cost_usd > 0)
        s->cost_usd = meta.cost_usd;

    if (meta.context_window > 0)
        s->context_window = meta.context_window;
    if (meta.context_tokens > 0)
        s->context_tokens = meta.context_tokens;

    /* Background workers the turn started are still going, so the turn ending
     * is not the tab going idle. */
    tab_busy(s, session_idle_busy(s));

    /* The turn is the likeliest thing to have touched the tree. */
    gitinfo_forget();

    update_title(s);
    remember_model(s);
    remember_window(s);
    if (!s->quiet)
        print_footer(s, elapsed);
    return 1;
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
const char *session_backend(const struct session *s) { return s->backend; }
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

void session_report(const struct session *s)
{
    char used[32], window[32];
    humanize(s->context_tokens, used, sizeof used);
    humanize(s->context_window, window, sizeof window);

    const char *auth = auth_description(s);
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
    if (s->id[0])
        ui_note("  session  %s", s->id);

    char scratch[512];
    const char *dir = s->cwd ? shorten_path(s, s->cwd, scratch, sizeof scratch) : ".";
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
    ui_put("\n");
    ui_flush();
}
