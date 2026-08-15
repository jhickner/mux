#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "agenttabs.h"
#include "banner.h"
#include "prompt.h"
#include "filediff.h"
#include "image.h"
#include "md.h"
#include "settings.h"
#include "status.h"
#include "title.h"
#include "toolstyle.h"
#include "transcript.h"
#include "tty.h"
#include "ui.h"
#include "vendor/agents/backend.h"
#include "vendor/cJSON.h"

struct session {
    Backend *agent;
    char    *backend;  /* driver name: claude, codex, grok, pi     */
    char    *cwd;
    char    *model;    /* requested; NULL means the CLI's default  */
    char    *effort;   /* requested; NULL means the CLI's default  */
    char    *resolved; /* what the CLI reported at init            */
    char     id[128];
    char     title[128]; /* the short name, once the helper has written one */
    int      retitle;    /* name the conversation again, ignoring the cached one */
    int      named;      /* the helper has been asked for this conversation's name */
    double   named_at;   /* when the cache was last read for it */
    char    *prompt;     /* the turn in flight, which is what it gets named for */
    char    *last_reply;
    char    *failed_prompt; /* last prompt whose provider turn failed */
    struct transcript transcript; /* completed turns for backend handoff */
    char    *last_block;  /* the final streamed text block of a turn  */
    char    *streamed;    /* every text block of a turn, in order     */
    int      turns;
    double   cost_usd;
    long     context_tokens;
    long     context_window;
    int      quiet;          /* suppress activity, spinner, and footer      */
    char     footer[384];    /* the last turn's summary row                 */
    int      hold_footer;    /* keep it for the HUD instead of printing it  */
    int      thinking;       /* show the model's reasoning rows             */
    int      compact;        /* one row per tool call, whatever it does     */
    int      customizations; /* load the user's skills, CLAUDE.md, MCP, ... */
    char    *permission;     /* --permission-mode; NULL means the default   */
    int      after_activity; /* last thing printed was a tool/thinking row  */
    int      after_tool;     /* ... a tool or thinking block that wants air */
    int      after_collapse; /* ... and specifically a collapsed tool row   */

    /* The run of collapsed calls sharing the row above the cursor. */
    struct {
        char  tool[64];
        char *line;    /* the row as printed, before styling */
        int   columns; /* the width it was fitted to         */
        int   onscreen;
    } cluster;
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

static double now_seconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
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
    static const char *const KEYS[] = {"command",          "file_path", "target_file",
                                       "path",             "target_directory",
                                       "pattern",          "url",       "query",
                                       "prompt",           "description",
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
    static const char *const KEYS[] = {"file_path", "target_file", "path",
                                       "notebook_path"};
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

/* Everything the agent does is indented; only what it says starts at column 0.
 * That is the whole difference between a tool block and a reply, so it has to
 * hold for every activity row — full, collapsed, and the ✻ reasoning rows. */
#define TOOL_INDENT 2

static void pad(int cells)
{
    for (int i = 0; i < cells; i++)
        ui_put(" ");
}

/* A reasoning block: the ✻ in chrome, the full text in the thinking color,
 * wrapped under the marker the same way a tool argument sits under its tag. */
static void print_activity(const char *marker, const char *text, enum ui_role role)
{
    int indent = TOOL_INDENT + (int)ui_cells(marker) + 1;
    int columns = ui_columns();
    int budget = columns - indent;
    if (budget < 8)
        budget = 8;

    pad(TOOL_INDENT);
    ui_esc(ui_style(UI_CHROME));
    ui_put(marker);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");

    if (!text || !*text) {
        ui_put("\n");
        return;
    }

    const char *style = ui_style(role);
    const char *p = text;
    int first = 1;
    while (*p) {
        size_t skip = 0;
        size_t row = ui_wrap_row(p, (size_t)budget, &skip);
        if (!first)
            pad(indent);
        if (*style)
            ui_esc(style);
        ui_putn(p, row);
        if (*style)
            ui_esc(ui_style(UI_RESET));
        ui_put("\n");
        p += row + skip;
        first = 0;
    }
}

static void tool_tag(const char *name, char *out, size_t size)
{
    size_t t = 0;
    out[t++] = '[';
    for (const char *p = name; *p && t + 2 < size; p++)
        out[t++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : *p;
    out[t++] = ']';
    out[t] = '\0';
}

/* Render a tool call as "[bash] <command>": the tag in its own color, the full
 * argument in normal weight so it stands out, wrapped under the tag. */
static void print_tool_call(const char *name, const char *arg)
{
    char tag[64];
    tool_tag(name, tag, sizeof tag);

    int indent = TOOL_INDENT + (int)ui_cells(tag) + 1;
    int columns = ui_columns();

    pad(TOOL_INDENT);
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
            pad(indent);
        ui_putn(p, row);
        ui_put("\n");
        p += row + skip;
        first = 0;
    }
}

/* ---------- collapsed tool rows ---------- */

/* A call that only looks at the workspace is worth a row, not a block: the tag
 * and its argument use the normal tool-call colors, with no output preview, and
 * a run of calls to the same tool listed together on one row. Compact mode puts
 * every call here, whatever it does.
 *
 * The row grows by being written again over itself. That works because every
 * print here happens with the status block lifted, which leaves the cursor at
 * the start of the row directly below the row to replace — and because the row
 * is kept inside the terminal width, so it never wrapped into two. */

static void cluster_forget(struct session *s)
{
    free(s->cluster.line);
    s->cluster.line = NULL;
    s->cluster.tool[0] = '\0';
    s->cluster.onscreen = 0;
}

/* Cells the row may fill: one short of the width so the terminal is never left
 * holding a deferred wrap, and one more in hand for the ellipsis. */
static int cluster_budget(void)
{
    int budget = ui_columns() - TOOL_INDENT - 2;
    return budget < 8 ? 8 : budget;
}

static void cluster_start(struct session *s, const char *name, const char *arg)
{
    char tag[64];
    tool_tag(name, tag, sizeof tag);

    int budget = cluster_budget() - (int)ui_cells(tag) - 1;
    if (budget < 8)
        budget = 8;

    size_t skip = 0;
    size_t fit = arg && *arg ? ui_wrap_row(arg, (size_t)budget, &skip) : 0;

    char row[4096];
    snprintf(row, sizeof row, "%s %.*s%s", tag, (int)fit, arg ? arg : "",
             arg && arg[fit] ? "…" : "");

    cluster_forget(s);
    snprintf(s->cluster.tool, sizeof s->cluster.tool, "%s", name);
    s->cluster.line = strdup(row);
    s->cluster.columns = ui_columns();
}

/* List another call on the open row. Fails when the row belongs to another
 * tool, was fitted to another width, or has no room left. */
static int cluster_extend(struct session *s, const char *name, const char *arg)
{
    if (!s->cluster.line || !s->after_collapse || strcmp(s->cluster.tool, name) != 0 ||
        s->cluster.columns != ui_columns())
        return 0;
    if (!arg || !*arg)
        return 1;

    char row[4096];
    snprintf(row, sizeof row, "%s, %s", s->cluster.line, arg);
    if ((int)ui_cells(row) > cluster_budget())
        return 0;

    free(s->cluster.line);
    s->cluster.line = strdup(row);
    return 1;
}

static void cluster_paint(struct session *s)
{
    if (!s->cluster.line)
        return;
    size_t tag = strcspn(s->cluster.line, "]");
    if (s->cluster.line[tag])
        tag++;

    if (s->cluster.onscreen)
        ui_esc("\x1b[1A\r\x1b[K");
    pad(TOOL_INDENT);
    ui_esc(ui_style(UI_TOOL));
    ui_putn(s->cluster.line, tag);
    ui_esc(ui_style(UI_RESET));
    ui_put(s->cluster.line + tag);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
    s->cluster.onscreen = 1;
}

/* Preview of a tool's output: the first few lines, dimmed, with a count of
 * whatever is left. */
#define TOOL_PREVIEW_ROWS 3

static void print_tool_output(const char *text, enum ui_role role)
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
            ui_esc(ui_style(role));
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

/* The session with a turn in flight, for the hooks the backend polls without a
 * user-data argument. */
static struct session *live;

static void remember_model(const struct session *s);
static void remember_choice(const char *what, const char *backend, const char *value);
static void await_model(struct session *s);

static void on_event(void *ud, const backend_event *ev)
{
    struct session *s = ud;

    if (ev->kind == BACKEND_EV_INIT) {
        replace(&s->resolved, ev->name);
        return;
    }
    /* Handshake and resume can emit events with no turn in flight. */
    if (s->quiet || !live)
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
        cluster_forget(s);
        s->after_activity = 0;
        s->after_tool = 0;
        s->after_collapse = 0;
        break;

    case BACKEND_EV_THINKING:
        if (!s->thinking || !ev->text || !*ev->text)
            break;
        status_pause();
        cluster_forget(s);
        if (s->after_tool) /* same blank as between consecutive tool blocks */
            ui_put("\n");
        print_activity("\xe2\x9c\xbb", ev->text, UI_THINKING); /* ✻ */
        status_resume();
        s->after_activity = 1;
        s->after_tool = 1;
        s->after_collapse = 0;
        break;

    case BACKEND_EV_TOOL: {
        const char *name = ev->name ? ev->name : "?";
        char arg[4096];
        tool_argument(s, ev, arg, sizeof arg);
        int collapsed = s->compact || toolstyle_collapses(name, ev->input_json, ev->arg);

        status_pause();
        if (collapsed) {
            if (!cluster_extend(s, name, arg)) {
                if (s->after_tool && !s->after_collapse) /* clear of the block above */
                    ui_put("\n");
                cluster_start(s, name, arg);
            }
            cluster_paint(s);
        } else {
            cluster_forget(s);
            if (s->after_tool) /* one blank row between consecutive spaced blocks */
                ui_put("\n");
            print_tool_call(name, arg);
        }
        status_resume();

        char path[4096];
        if (!collapsed && tool_path(s, ev->input_json, path, sizeof path))
            filediff_snapshot(path);
        else
            filediff_clear();

        s->after_activity = 1;
        s->after_tool = 1;
        s->after_collapse = collapsed;
        break;
    }

    case BACKEND_EV_TOOL_RESULT:
        if (ev->failed) {
            status_pause();
            cluster_forget(s);
            filediff_clear();
            {
                const char *why = ev->text && *ev->text ? ev->text : NULL;
                if (!why || !strcmp(why, "failed")) {
                    print_tool_output("failed", UI_ERROR);
                } else {
                    char line[4096];
                    snprintf(line, sizeof line, "failed: %s", why);
                    print_tool_output(line, UI_ERROR);
                }
            }
            status_resume();
            s->after_activity = 1;
            s->after_tool = 1;
            s->after_collapse = 0;
            break;
        }
        /* A collapsed call has already said everything it is going to; leaving
         * the row untouched is also what keeps the next one able to rewrite it. */
        if (!s->after_collapse) {
            status_pause();
            /* A tool that changed the file speaks for itself; one that did not
             * still needs its own output shown. */
            int drew;
            if (ev->diff) {
                drew = filediff_render_patch(ev->diff);
                filediff_clear();
            } else {
                drew = filediff_render();
            }
            if (!drew)
                print_tool_output(ev->text, UI_DIM);
            status_resume();
        }
        s->after_activity = 1;
        s->after_tool = 1;
        break;
    }
    /* Tool and thinking rows end without a trailing blank, so the spinner would
     * otherwise sit flush against them. The row is part of the status block. */
    status_gap(s->after_tool);
    ui_flush();
}

int session_idle_fd(const struct session *s)
{
    if (!s || !s->agent || !s->agent->idle_fd || s->quiet)
        return -1;
    return s->agent->idle_fd(s->agent);
}

void session_idle_pump(struct session *s)
{
    if (!s || !s->agent || !s->agent->idle_pump || s->quiet)
        return;
    /* on_event prints only for the turn in flight, and this is a turn nobody
     * asked for: name it live for the pump so it reaches the screen.
     *
     * A collapsed tool row is rewritten in place as the cluster grows, and each
     * pump hands the screen back to the caller's display in between, so the row
     * a cluster would rewrite is no longer the one under the cursor. Starting
     * every pump clean costs a merged row and keeps the rewrites off scrollback
     * that has already moved on. */
    cluster_forget(s);
    s->after_activity = 0;
    s->after_tool = 0;
    s->after_collapse = 0;
    live = s;
    s->agent->idle_pump(s->agent);
    live = NULL;
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

/* A conversation is worth naming while its first turn is still running, so the
 * helper is started as soon as the CLI reports the session id and the answer is
 * picked up off the cache a second or so later — both from here, since a turn
 * can run for minutes without an event of its own. */
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

/* Requested level if the user set one; otherwise whatever the backend has
 * resolved (Claude's settings.json / stream `effort`, GROK_EFFORT, ...). */
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

/* Polled by the backend many times a second: drain whatever was typed, then
 * tick the spinner, and report whether the user asked to interrupt. This is the
 * only place keys are read during a turn, so its cadence is what echo latency
 * costs — hence the short poll timeouts in the drivers. */
static int abort_check(void)
{
    name_poll(live);
    quota_poll(live);
    if (live)
        set_spin_word(live);

    /* Without a raw terminal there is no interrupt key to read, and reading a
     * redirected stdin would see EOF and cancel the turn immediately. */
    if (!tty_is_raw())
        return 0;

    int interrupt = 0;
    tty_event ev;
    while (tty_read(&ev, 0)) {
        /* Typing edits the live prompt, and a resize rewraps it: either way the
         * block on screen is stale until the tick below repaints it. */
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

/* ---------- lifecycle ---------- */

struct session *session_new(const char *backend, const char *cwd, const char *model,
                            const char *effort)
{
    struct session *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->backend = strdup(backend && *backend ? backend : "claude");
    s->cwd = cwd ? strdup(cwd) : NULL;
    s->model = model ? strdup(model) : NULL;
    s->effort = effort ? strdup(effort) : NULL;
    s->thinking = 1;
    return s;
}

/* TEMP: repaint the conversation in the current colors. Only the retained turns
 * come back — tool activity, reasoning and footers are not kept — so this is a
 * try-out aid, not a faithful redraw. Remove with the Ctrl-N/Ctrl-O binds. */
void session_replay(struct session *s)
{
    ui_esc("\x1b[2J\x1b[H");
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
    free(s->streamed);
    free(s->prompt);
    free(s->permission);
    free(s->cluster.line);
    transcript_free(&s->transcript);
    if (live == s)
        live = NULL;
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
    o.effort = s->effort;
    o.allow_customizations = s->customizations;
    o.permission_mode = s->permission;
    /* The model has no way to discover the front end can draw, so it is told.
     * Only when it actually can: the instruction is worse than useless in a
     * terminal that would print the markdown instead. */
    if (img_available())
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

int session_switch_backend(struct session *s, const char *backend)
{
    if (!s || !backend || !*backend)
        return 0;
    if (strcmp(s->backend, backend) == 0)
        return 1;

    /* Keep enough recent verbatim dialogue for a faithful handoff without
     * risking an enormous argv/developer-instructions field. */
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

    /* Starting is transactional: do not tear down a usable provider until its
     * replacement has completed its own handshake. */
    if (!replacement->start(replacement, NULL)) {
        replacement->close(replacement);
        return 0;
    }
    replacement->set_event_cb(replacement, on_event, s);
    replacement->set_abort_check(replacement, abort_check);

    Backend *previous = s->agent;
    s->agent = replacement;
    replace(&s->backend, backend);
    replace(&s->model, NULL);       /* provider model names are not portable */
    replace(&s->effort, NULL);      /* nor are its effort levels */
    replace(&s->resolved, NULL);
    s->id[0] = '\0';
    const char *id = replacement->session_id(replacement);
    if (id) {
        /* This is a new provider leg of the same logical conversation. Keep
         * its title instead of treating the provider's new id as a new topic. */
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
    return 1;
}

void session_set_quiet(struct session *s, int quiet) { s->quiet = quiet; }

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

/* Adopting a session id also disowns whatever the plugin's hook recorded for
 * that same conversation in an earlier run. */
static void set_id(struct session *s, const char *id)
{
    /* Re-adopting the same id is routine — it arrives again at the end of every
     * turn — and must not restart the naming that is already under way. */
    int changed = strcmp(s->id, id) != 0;
    snprintf(s->id, sizeof s->id, "%s", id);
    if (changed) {
        /* A resumed conversation carries its name in from the start; one waiting
         * to be renamed keeps none until the helper has written the new one. */
        s->title[0] = '\0';
        s->named = 0;
        if (!s->retitle)
            title_lookup(s->id, s->title, sizeof s->title);
        status_set_note(s->title);
    }
    agenttabs_forget_hook(id);
}

/* (Re)start the child process, carrying the conversation across on the backends
 * that can resume one. */
static int restart(struct session *s, const char *resume_id)
{
    Backend *b = agent(s);
    if (!b || !b->start(b, resume_id))
        return 0;
    if (resume_id && resume_id != s->id)
        set_id(s, resume_id);
    /* Adopt an id the backend already knows (a resume, or one reported without
     * waiting on the child). A fresh pi id arrives after the first turn. */
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

    char *previous = s->model;
    s->model = model ? strdup(model) : NULL;
    b->set_model(b, s->model);
    if (restart(s, s->id[0] ? s->id : NULL)) {
        free(previous);
        replace(&s->resolved, NULL);
        remember_choice("model", s->backend, s->model);
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
        remember_choice("effort", s->backend, s->effort);
        return 1;
    }
    free(s->effort);
    s->effort = previous;
    b->set_effort(b, s->effort);
    return 0;
}

/* Index 0 is what the CLI gets when nothing is stored. */
static const char *const PERMISSIONS[] = {
    "bypassPermissions", "auto", "acceptEdits", "dontAsk", "manual", "plan",
};
#define PERMISSION_COUNT ((int)(sizeof PERMISSIONS / sizeof *PERMISSIONS))

const char *session_permission_name(int index)
{
    return (index >= 0 && index < PERMISSION_COUNT) ? PERMISSIONS[index] : NULL;
}

int session_permission_index(const char *mode)
{
    for (int i = 0; mode && i < PERMISSION_COUNT; i++)
        if (strcmp(PERMISSIONS[i], mode) == 0)
            return i;
    return -1;
}

const char *session_permission(const struct session *s)
{
    return (s && s->permission) ? s->permission : PERMISSIONS[0];
}

int session_set_permission(struct session *s, const char *mode)
{
    /* Before the first start there is nothing to restart: the mode is read at
     * open time, so storing it is enough and avoids spawning the CLI twice. */
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
    /* Whatever it is about now, it is not what it was named for. Set before
     * set_id, which otherwise reads the old name back out of the cache. */
    s->title[0] = '\0';
    s->retitle = 1;
    s->named = 0;
    status_set_note(NULL);
    /* A cleared conversation gets a new session id from the CLI. */
    const char *id = s->agent->session_id(s->agent);
    if (id)
        set_id(s, id);
    else
        s->id[0] = '\0';
    return 1;
}

/* ---------- one turn ---------- */

/* What name_poll could not finish while the turn ran: the id only turns up at
 * the end on a backend that reports it late, and a helper that answered in the
 * last second of the turn was never polled for. */
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

/* The turn summary — elapsed, context, cost, name — kept on the session so the
 * HUD can carry it in the row the spinner just left, and printed into scrollback
 * only when nothing is holding it there. */
static void print_footer(struct session *s, double elapsed)
{
    char used[32], window[32];
    humanize(s->context_tokens, used, sizeof used);
    humanize(s->context_window, window, sizeof window);

    /* snprintf reports what it *would* have written, so advancing by its return
     * value unchecked can walk past the buffer. */
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

    /* The name trails the figures, and drops to a row of its own rather than let
     * the terminal wrap it mid-word. The spare column keeps a full row from
     * leaving a deferred wrap behind. */
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
        ui_wrapped(s->title, 0, ui_style(UI_DIM)); /* ends its own row */
    ui_put("\n");
    ui_flush();
}

int session_turn(struct session *s, const char *text)
{
    if (!s->agent)
        return 0;

    replace(&s->last_block, NULL);
    replace(&s->streamed, NULL);
    replace(&s->prompt, text);
    s->after_activity = 0;
    s->after_tool = 0;
    s->after_collapse = 0;
    cluster_forget(s);
    live = s;
    double started = now_seconds();
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
        agenttabs_errored();
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
    /* Only the latest primary-model request says how much of the window is
     * occupied. The result event's usage block is aggregate traffic across
     * every model request in the turn — a long tool loop re-reads the same
     * cached prompt and reports several windows' worth — so a driver that
     * reports no per-request figure leaves the last real one standing rather
     * than overwrite it with a number that measures something else. An
     * interrupted turn reports a stub usage block, which this skips for free.
     * The window is a property of the model, so it stands on its own. */
    if (meta.context_window > 0)
        s->context_window = meta.context_window;
    if (meta.context_tokens > 0)
        s->context_tokens = meta.context_tokens;

    /* Interrupting is a turn that ended, not one that failed — and it is the
     * case the CLI's own hook never reports. */
    agenttabs_finished();

    update_title(s);
    remember_model(s);
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

const char *session_effort(const struct session *s)
{
    return s->effort && *s->effort ? s->effort : "default";
}

/* Where the id a backend resolved for this model request is remembered between
 * runs, so the banner can name the model without waiting on a handshake. */
static void model_cache_key(const struct session *s, char *out, size_t n)
{
    snprintf(out, n, "model.%s.%s", s->backend,
             s->model && *s->model ? s->model : "default");
}

/* And where the choices themselves are kept — one of each per backend, since
 * neither a model name nor an effort level is portable across them. */
static void choice_key(const char *what, const char *backend, char *out, size_t n)
{
    snprintf(out, n, "%s.%s", what, backend);
}

/* "default" is stored for the pick that asks for nothing, so that choosing it
 * back is remembered too rather than reading as "never picked". */
static const char *saved_choice(const char *what, const char *backend)
{
    if (!backend || !*backend)
        return NULL;
    char key[MAX_SETTING_KEY];
    choice_key(what, backend, key, sizeof key);
    const char *saved = settings_get_str(key, NULL);
    return saved && strcmp(saved, "default") != 0 ? saved : NULL;
}

static void remember_choice(const char *what, const char *backend, const char *value)
{
    char key[MAX_SETTING_KEY];
    choice_key(what, backend, key, sizeof key);
    settings_set_str(key, value && *value ? value : "default");
}

const char *session_saved_model(const char *backend)
{
    return saved_choice("model", backend);
}

const char *session_saved_effort(const char *backend)
{
    return saved_choice("effort", backend);
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
    if (!id || !*id)
        return;
    char key[MAX_SETTING_KEY];
    model_cache_key(s, key, sizeof key);
    settings_set_str(key, id);
}

/* A model change restarts the CLI, whose handshake answers with the id the new
 * request resolves to a moment later. Waiting for it here is what lets the
 * switch be recorded — and named on screen — without a turn having run. */
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

const char *session_model_label(const struct session *s)
{
    if (s->resolved && *s->resolved)
        return s->resolved;
    /* The backend has this as soon as its own startup handshake lands, which is
     * usually after the banner has been drawn — hence the remembered id. */
    if (s->agent && s->agent->model) {
        const char *got = s->agent->model(s->agent);
        if (got && *got)
            return got;
    }
    char key[MAX_SETTING_KEY];
    model_cache_key(s, key, sizeof key);
    const char *cached = settings_get_str(key, NULL);
    if (cached)
        return cached;
    if (s->model && *s->model)
        return s->model;
    return "default";
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
    /* Mid-turn the driver has a newer count than the last footer; the window
     * only arrives with the turn's final event, so keep the one we have. */
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
    if (session_can_set_effort(s))
        ui_note("  effort   %s", session_effort(s));
    if (auth)
        ui_note("  auth     %s", auth);
    /* Only Claude Code is told whether to load the user's own configuration. */
    if (strcmp(s->backend, "claude") == 0) {
        ui_note("  config   %s", s->customizations ? "skills, CLAUDE.md, MCP, agents"
                                                   : "safe mode (customizations off)");
        ui_note("  tools    %s", session_permission(s));
    }
    ui_note("  calls    %s", s->compact ? "compact (one row each)" : "full blocks");
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
