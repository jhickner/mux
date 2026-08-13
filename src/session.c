#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "filediff.h"
#include "md.h"
#include "status.h"
#include "tty.h"
#include "ui.h"
#include "vendor/agents/backend.h"
#include "vendor/cJSON.h"

struct session {
    Backend *agent;
    char    *backend;  /* driver name: claude, codex, grok, pi     */
    char    *cwd;
    char    *model;    /* requested; NULL means the CLI's default  */
    char    *resolved; /* what the CLI reported at init            */
    char     id[128];
    char    *last_reply;
    char    *last_block;  /* the final streamed text block of a turn  */
    char    *streamed;    /* every text block of a turn, in order     */
    int      turns;
    double   cost_usd;
    long     context_tokens;
    long     context_window;
    int      quiet;          /* suppress activity, spinner, and footer      */
    int      thinking;       /* show the model's reasoning rows             */
    int      customizations; /* load the user's skills, CLAUDE.md, MCP, ... */
    int      after_activity; /* last thing printed was a tool/thinking row  */
    int      after_tool;     /* ... and specifically a tool call block      */
};

/* ---------- small helpers ---------- */

static void replace(char **slot, const char *value)
{
    free(*slot);
    *slot = value ? strdup(value) : NULL;
}

static void append(char **slot, const char *value)
{
    if (!value || !*value)
        return;
    size_t have = *slot ? strlen(*slot) : 0, add = strlen(value);
    char *grown = realloc(*slot, have + add + 1);
    if (!grown)
        return;
    memcpy(grown + have, value, add + 1);
    *slot = grown;
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

/* Collapse whitespace runs and clip to `max` bytes so a label stays one line. */
static void one_line(const char *in, char *out, size_t max)
{
    size_t o = 0;
    int space = 0;
    for (const char *p = in; *p && o + 1 < max; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n' || c == '\t' || c == '\r' || c == ' ') {
            if (o == 0 || space)
                continue;
            space = 1;
            out[o++] = ' ';
            continue;
        }
        space = 0;
        out[o++] = (char)c;
    }
    while (o > 0 && out[o - 1] == ' ')
        o--;
    out[o] = '\0';
}

/* Absolute paths under the agent's cwd (or $HOME) are noise at full length. */
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

/* What a tool call is doing: the telling field of its JSON input, or the
 * one-line summary a backend that reports no structured input handed us. */
static void tool_argument(const struct session *s, const backend_event *ev, char *out, size_t size)
{
    static const char *const KEYS[] = {"command",     "file_path", "path",   "pattern",
                                       "url",         "query",     "prompt", "description",
                                       "notebook_path"};
    char arg[4096] = "";

    if (ev->input_json) {
        cJSON *input = cJSON_Parse(ev->input_json);
        if (input) {
            for (size_t i = 0; i < sizeof KEYS / sizeof *KEYS; i++) {
                const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(input, KEYS[i]));
                if (v && *v) {
                    char scratch[1024];
                    one_line(shorten_path(s, v, scratch, sizeof scratch), arg, sizeof arg);
                    break;
                }
            }
            cJSON_Delete(input);
        }
    } else if (ev->arg) {
        one_line(ev->arg, arg, sizeof arg);
    }
    snprintf(out, size, "%s", arg);
}

/* The file a tool is about to touch, made absolute against the agent's cwd.
 * Keyed on the argument name rather than the tool name, so a backend with its
 * own tool vocabulary still gets a diff. Returns 0 when there is no path. */
static int tool_path(const struct session *s, const char *input_json, char *out, size_t size)
{
    static const char *const KEYS[] = {"file_path", "path", "notebook_path"};
    if (!input_json)
        return 0;

    cJSON *input = cJSON_Parse(input_json);
    if (!input)
        return 0;

    const char *found = NULL;
    for (size_t i = 0; i < sizeof KEYS / sizeof *KEYS && !found; i++) {
        const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(input, KEYS[i]));
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

/* ---------- live turn rendering ---------- */

static void print_activity(const char *marker, const char *text, enum ui_role role)
{
    int columns = ui_columns();
    int budget = columns - 6;
    if (budget < 8)
        budget = 8;

    char clipped[512];
    one_line(text, clipped, sizeof clipped);

    size_t skip = 0;
    size_t fit = ui_wrap_row(clipped, (size_t)budget, &skip);

    ui_put("  ");
    ui_esc(ui_style(UI_CHROME));
    ui_put(marker);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");
    ui_esc(ui_style(role));
    ui_putn(clipped, fit);
    if (clipped[fit])
        ui_put("…");
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

/* Render a tool call as "[bash] <command>": the tag in its own color, the full
 * argument in normal weight so it stands out, wrapped under the tag. */
static void print_tool_call(const char *name, const char *arg)
{
    char tag[64];
    size_t t = 0;
    tag[t++] = '[';
    for (const char *p = name; *p && t + 2 < sizeof tag; p++)
        tag[t++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : *p;
    tag[t++] = ']';
    tag[t] = '\0';

    int indent = (int)ui_cells(tag) + 1;
    int columns = ui_columns();

    ui_esc(ui_style(UI_TOOL));
    ui_put(tag);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");

    if (!arg || !*arg) {
        ui_put("\n");
        return;
    }
    int budget = columns - indent;
    if (budget < 8)
        budget = 8;
    const char *p = arg;
    int first = 1;
    while (*p) {
        size_t skip = 0;
        size_t row = ui_wrap_row(p, (size_t)budget, &skip);
        if (!first)
            for (int i = 0; i < indent; i++)
                ui_put(" ");
        ui_putn(p, row);
        ui_put("\n");
        p += row + skip;
        first = 0;
    }
}

/* Preview of a tool's output: the first few lines, dimmed, with a count of
 * whatever is left. */
#define TOOL_PREVIEW_ROWS 3

static void print_tool_output(const char *text)
{
    if (!text || !*text)
        return;

    int columns = ui_columns();
    int budget = columns - 6;
    if (budget < 8)
        budget = 8;

    const char *p = text;
    int shown = 0;
    while (*p && shown < TOOL_PREVIEW_ROWS) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char line[1024];
        if (n >= sizeof line)
            n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = '\0';

        char clipped[1024];
        one_line(line, clipped, sizeof clipped);
        if (*clipped) {
            size_t skip = 0;
            size_t fit = ui_wrap_row(clipped, (size_t)budget, &skip);
            ui_put("    ");
            ui_esc(ui_style(UI_DIM));
            ui_putn(clipped, fit);
            if (clipped[fit])
                ui_put("…");
            ui_esc(ui_style(UI_RESET));
            ui_put("\n");
            shown++;
        }
        if (!nl)
            break;
        p = nl + 1;
    }

    int remaining = 0;
    for (const char *q = p; *q; q++)
        if (*q == '\n' && q[1])
            remaining++;
    if (*p && shown >= TOOL_PREVIEW_ROWS)
        remaining++;
    if (remaining > 0) {
        ui_put("    ");
        ui_esc(ui_style(UI_DIM));
        ui_printf("+%d line%s", remaining, remaining == 1 ? "" : "s");
        ui_esc(ui_style(UI_RESET));
        ui_put("\n");
    }
}

static void on_event(void *ud, const backend_event *ev)
{
    struct session *s = ud;

    if (ev->kind == BACKEND_EV_INIT) {
        replace(&s->resolved, ev->name);
        return;
    }
    if (s->quiet)
        return;

    switch (ev->kind) {
    case BACKEND_EV_INIT:
        break;

    case BACKEND_EV_ASSISTANT:
        if (!ev->text || !*ev->text)
            break;
        status_pause();
        /* Let the prose breathe after a run of tool rows. */
        if (s->after_activity)
            ui_put("\n");
        md_render(ev->text, 0);
        ui_put("\n");
        status_resume();
        replace(&s->last_block, ev->text);
        append(&s->streamed, ev->text);
        s->after_activity = 0;
        s->after_tool = 0;
        break;

    case BACKEND_EV_THINKING:
        if (!s->thinking || !ev->text || !*ev->text)
            break;
        status_pause();
        print_activity("\xe2\x9c\xbb", ev->text, UI_DIM); /* ✻ */
        status_resume();
        s->after_activity = 1;
        s->after_tool = 0;
        break;

    case BACKEND_EV_TOOL: {
        const char *name = ev->name ? ev->name : "?";
        char arg[4096];
        tool_argument(s, ev, arg, sizeof arg);
        status_pause();
        if (s->after_tool) /* one blank row between consecutive tool blocks */
            ui_put("\n");
        print_tool_call(name, arg);
        status_resume();

        char path[4096];
        if (tool_path(s, ev->input_json, path, sizeof path))
            filediff_snapshot(path);
        else
            filediff_clear();

        char label[256];
        snprintf(label, sizeof label, "%s %s", name, arg);
        status_activity(label);
        s->after_activity = 1;
        s->after_tool = 1;
        break;
    }

    case BACKEND_EV_TOOL_RESULT:
        status_pause();
        /* A tool that changed the file speaks for itself; one that did not (a
         * read, a failed edit) still needs its own output shown. */
        if (!filediff_render())
            print_tool_output(ev->text);
        status_resume();
        status_activity(NULL);
        s->after_activity = 1;
        s->after_tool = 1;
        break;
    }
    ui_flush();
}

/* Where keys typed during a turn go. The abort predicate the backend polls
 * takes no argument, so the hook lives here rather than on the session. */
static session_key_fn typeahead;
static void          *typeahead_ud;

void session_set_typeahead(session_key_fn fn, void *ud)
{
    typeahead = fn;
    typeahead_ud = ud;
}

/* Polled by the backend a few times a second: drain whatever was typed, then
 * tick the spinner, and report whether the user asked to interrupt. */
static int abort_check(void)
{
    /* Without a raw terminal there is no interrupt key to read, and reading a
     * redirected stdin would see EOF and cancel the turn immediately. */
    if (!tty_is_raw())
        return 0;

    int interrupt = 0;
    tty_event ev;
    while (tty_read(&ev, 0)) {
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

/* ---------- lifecycle ---------- */

struct session *session_new(const char *backend, const char *cwd, const char *model)
{
    struct session *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->backend = strdup(backend && *backend ? backend : "claude");
    s->cwd = cwd ? strdup(cwd) : NULL;
    s->model = model ? strdup(model) : NULL;
    s->thinking = 1;
    return s;
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
    free(s->resolved);
    free(s->last_reply);
    free(s->last_block);
    free(s->streamed);
    free(s);
}

/* Open the backend on first use. Everything the driver is told at open time —
 * the name, the working directory, whether to load the user's customizations —
 * is fixed here, so this runs after the setters and before the first start. */
static Backend *agent(struct session *s)
{
    if (s->agent)
        return s->agent;
    backend_opts o = {0};
    o.name = s->backend;
    o.cwd = s->cwd;
    o.model = s->model;
    o.allow_customizations = s->customizations;
    s->agent = backend_open_ex(&o);
    if (s->agent) {
        s->agent->set_event_cb(s->agent, on_event, s);
        s->agent->set_abort_check(s->agent, abort_check);
    }
    return s->agent;
}

void session_set_quiet(struct session *s, int quiet) { s->quiet = quiet; }

void session_set_thinking(struct session *s, int on) { s->thinking = on; }

int session_thinking(const struct session *s) { return s->thinking; }

void session_set_customizations(struct session *s, int on) { s->customizations = on; }

/* (Re)start the child process, carrying the conversation across on the backends
 * that can resume one. */
static int restart(struct session *s, const char *resume_id)
{
    Backend *b = agent(s);
    if (!b || !b->start(b, resume_id))
        return 0;
    if (resume_id && resume_id != s->id)
        snprintf(s->id, sizeof s->id, "%s", resume_id);
    return 1;
}

int session_start(struct session *s) { return restart(s, s->id[0] ? s->id : NULL); }

int session_set_model(struct session *s, const char *model)
{
    Backend *b = agent(s);
    if (!b)
        return 0;

    char *previous = s->model;
    s->model = model ? strdup(model) : NULL;
    b->set_model(b, s->model);
    if (restart(s, s->id[0] ? s->id : NULL)) {
        free(previous);
        replace(&s->resolved, NULL);
        return 1;
    }
    free(s->model);
    s->model = previous;
    b->set_model(b, s->model);
    return 0;
}

void session_adopt_id(struct session *s, const char *id)
{
    if (s && id && *id)
        snprintf(s->id, sizeof s->id, "%s", id);
}

int session_resume(struct session *s, const char *id)
{
    if (!restart(s, id))
        return 0;
    s->turns = 0;
    s->cost_usd = 0;
    s->context_tokens = 0;
    replace(&s->last_reply, NULL);
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
    replace(&s->last_block, NULL);
    /* A cleared conversation gets a new session id from the CLI. */
    const char *id = s->agent->session_id(s->agent);
    if (id)
        snprintf(s->id, sizeof s->id, "%s", id);
    return 1;
}

static double now_seconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

/* ---------- one turn ---------- */

static void print_footer(const struct session *s, double elapsed)
{
    char used[32], window[32];
    humanize(s->context_tokens, used, sizeof used);
    humanize(s->context_window, window, sizeof window);

    /* snprintf reports what it *would* have written, so advancing by its return
     * value unchecked can walk past the buffer. */
    char line[256];
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
    #undef APPEND

    ui_esc(ui_style(UI_DIM));
    ui_put(line);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n\n");
    ui_flush();
}

int session_turn(struct session *s, const char *text)
{
    if (!s->agent)
        return 0;

    replace(&s->last_block, NULL);
    replace(&s->streamed, NULL);
    s->after_activity = 0;
    s->after_tool = 0;
    double started = now_seconds();
    if (!s->quiet)
        status_begin();

    backend_result meta = {0};
    char *reply = s->agent->ask_ex(s->agent, text, &meta);
    double elapsed = now_seconds() - started;
    if (!s->quiet)
        status_end();

    const char *id = s->agent->session_id(s->agent);
    if (id)
        snprintf(s->id, sizeof s->id, "%s", id);

    if (!reply) {
        const char *detail = s->agent->last_error(s->agent);
        if (detail)
            ui_error("%s: %s", s->backend, detail);
        else
            ui_error("the %s process stopped responding", s->backend);
        ui_put("\n");
        return 0;
    }

    /* The reply repeats what the stream already showed — the last block for the
     * backends that report one, everything streamed for those that hand back
     * the whole turn. Print only what neither covered. */
    int shown = (s->last_block && strcmp(reply, s->last_block) == 0) ||
                (s->streamed && strcmp(reply, s->streamed) == 0);
    if (s->quiet) {
        if (*reply) {
            ui_put(reply);
            ui_put("\n");
        }
    } else if (*reply && !shown) {
        if (s->after_activity)
            ui_put("\n");
        md_render(reply, 0);
        ui_put("\n");
    }
    if (meta.interrupted) {
        ui_note("  interrupted");
        ui_put("\n");
    }

    if (*reply)
        replace(&s->last_reply, reply);
    free(reply);

    s->turns++;
    if (meta.cost_usd > 0)
        s->cost_usd = meta.cost_usd;
    /* Result-event usage is aggregate traffic across every model request in
     * the turn and can exceed the context window many times over, so prefer
     * the latest primary-model request where the driver reports one.  The rest
     * only have the aggregate.  An interrupted turn reports a stub usage block
     * either way, so keep the last real number then. */
    long used = meta.context_tokens;
    if (used <= 0 && !meta.interrupted)
        used = meta.input_tokens + meta.output_tokens + meta.cache_read_tokens +
               meta.cache_creation_tokens;
    if (used > 0) {
        s->context_tokens = used;
        if (meta.context_window > 0)
            s->context_window = meta.context_window;
    }

    if (!s->quiet)
        print_footer(s, elapsed);
    return 1;
}

/* ---------- accessors ---------- */

const char *session_model(const struct session *s)
{
    if (s->resolved && *s->resolved)
        return s->resolved;
    if (s->model && *s->model)
        return s->model;
    return "default";
}

const char *session_id(const struct session *s) { return s->id[0] ? s->id : NULL; }

int session_can_resume(const struct session *s)
{
    return s->agent && (s->agent->caps & BACKEND_CAP_RESUME);
}

const char *session_cwd(const struct session *s) { return s->cwd; }
const char *session_backend(const struct session *s) { return s->backend; }
const char *session_last_reply(const struct session *s) { return s->last_reply; }

/* How the CLI authenticated, or NULL when the backend never says. A CLI that
 * reports "none" had no key env var in play, which is exactly the case where it
 * fell back to the subscription login. */
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
    if (auth)
        ui_note("  auth     %s", auth);
    /* Only Claude Code is told whether to load the user's own configuration. */
    if (strcmp(s->backend, "claude") == 0)
        ui_note("  config   %s", s->customizations ? "skills, CLAUDE.md, MCP, agents"
                                                   : "safe mode (customizations off)");
    if (s->id[0])
        ui_note("  session  %s", s->id);
    /* Keep the report to one row per field even in a narrow terminal.
     * shorten_path already rewrites a $HOME prefix as "~". */
    char scratch[512];
    const char *dir = s->cwd ? shorten_path(s, s->cwd, scratch, sizeof scratch) : ".";
    int room = ui_columns() - 12;
    size_t cells = ui_cells(dir);
    if (room > 8 && cells > (size_t)room) {
        /* Elide the head: the tail of a path is the identifying part. */
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
