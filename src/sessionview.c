#include "sessionview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filediff.h"
#include "highlight.h"
#include "text.h"
#include "toolstyle.h"
#include "viewport.h"
#include "vendor/cJSON.h"

#define TOOL_INDENT 2

#define COUNT(a) (sizeof (a) / sizeof *(a))

/* --- kept entries --------------------------------------------------------- */

// What a turn prints, kept as entries that redraw themselves at a new width.

enum keep_kind { KEEP_ACTIVITY, KEEP_CALL, KEEP_OUTPUT, KEEP_DIFF, KEEP_CLUSTER };

struct keep {
    enum keep_kind kind;
    char          *a;           /* marker, tool name, text or patch */
    char          *b;           /* the argument of a tool call */
    unsigned char *spans;       /* highlighting for a cluster row */
    enum ui_role   role;
    int            error;       /* output drawn as a failure */
    int            gap;         /* a blank row above */
};

static void keep_free(void *ud)
{
    struct keep *k = ud;
    free(k->a);
    free(k->b);
    free(k->spans);
    free(k);
}

static void cluster_paint(const char *line, const unsigned char *spans);

static void keep_render(void *ud, int cols)
{
    const struct keep *k = ud;
    (void)cols;

    if (k->gap)
        ui_put("\n");
    switch (k->kind) {
    case KEEP_ACTIVITY: view_activity(k->a, k->b, k->role);              break;
    case KEEP_CALL:     view_tool_call(k->a, k->b);                      break;
    case KEEP_OUTPUT:
        if (k->error)
            view_tool_error(k->a);
        else
            view_tool_output(k->a, k->role);
        break;
    case KEEP_DIFF:     filediff_render_patch(k->a);                     break;
    case KEEP_CLUSTER:  cluster_paint(k->a, k->spans);                   break;
    }
}

// Opens the entry, draws it once, closes it. Returns its mark.
static unsigned keep(struct keep *k)
{
    // Settled once, not per redraw: two blanks where the last entry left one.
    if (k->gap && viewport_ends_blank())
        k->gap = 0;

    unsigned mark = viewport_item_begin(keep_render, k, keep_free);
    keep_render(k, ui_columns());
    viewport_item_end();
    return mark;
}

static struct keep *keep_new(enum keep_kind kind)
{
    struct keep *k = calloc(1, sizeof *k);
    if (k)
        k->kind = kind;
    return k;
}

void view_keep_activity(const char *marker, const char *text, enum ui_role role, int gap)
{
    struct keep *k = keep_new(KEEP_ACTIVITY);
    if (!k)
        return;
    k->a = strdup(marker ? marker : "");
    k->b = strdup(text ? text : "");
    k->role = role;
    k->gap = gap;
    keep(k);
}

void view_keep_tool_call(const char *name, const char *arg, int gap)
{
    struct keep *k = keep_new(KEEP_CALL);
    if (!k)
        return;
    k->a = strdup(name ? name : "?");
    k->b = strdup(arg ? arg : "");
    k->gap = gap;
    keep(k);
}

void view_keep_output(const char *text, enum ui_role role, int error)
{
    if (!text || !*text)
        return;
    struct keep *k = keep_new(KEEP_OUTPUT);
    if (!k)
        return;
    k->a = strdup(text);
    k->role = role;
    k->error = error;
    keep(k);
}

// Takes the patch.
void view_keep_diff(char *patch)
{
    if (!patch || !*patch) {
        free(patch);
        return;
    }
    struct keep *k = keep_new(KEEP_DIFF);
    if (!k) {
        free(patch);
        return;
    }
    k->a = patch;
    keep(k);
}

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

static const char *shorten_path(const char *cwd, const char *value, char *scratch,
                                size_t size)
{
    if (value[0] != '/')
        return value;
    if (cwd) {
        size_t n = strlen(cwd);
        if (strncmp(value, cwd, n) == 0 && value[n] == '/')
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

void view_tool_argument(const backend_event *ev, const char *cwd, char *out, size_t size)
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
                    text_block(shorten_path(cwd, v, scratch, sizeof scratch), arg,
                               sizeof arg);
                    break;
                }
            }
            cJSON_Delete(input);
        }
    } else if (ev->arg) {
        text_block(ev->arg, arg, sizeof arg);
    }
    snprintf(out, size, "%s", arg);
}

int view_tool_path(const char *input_json, const char *cwd, char *out, size_t size)
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
            snprintf(out, size, "%s/%s", cwd ? cwd : ".", found);
    }
    cJSON_Delete(input);
    return found != NULL;
}

void view_activity(const char *marker, const char *text, enum ui_role role)
{
    int indent = TOOL_INDENT + (int)ui_cells(marker) + 1;
    int columns = ui_columns();
    int budget = columns - indent;
    if (budget < 8)
        budget = 8;

    ui_pad(TOOL_INDENT);
    ui_esc(ui_style(UI_CHROME));
    ui_put(marker);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");

    if (!text || !*text) {
        ui_put("\n");
        return;
    }

    struct ui_wrap w = {0};
    w.budget = (size_t)budget;
    w.indent = indent;
    w.role = role;
    ui_wrap_paint(text, &w);
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

#define TOOL_CALL_ROWS 24

/* Per-byte highlight roles for a shell command, or NULL for other tools. */
static unsigned char *shell_spans(const char *name, const char *text, size_t len)
{
    if (!toolstyle_is_shell(name) || !len)
        return NULL;

    unsigned char *spans = malloc(len);
    if (spans)
        highlight_shell(text, len, spans);
    return spans;
}

void view_tool_call(const char *name, const char *arg)
{
    char tag[64];
    tool_tag(name, tag, sizeof tag);

    int indent = TOOL_INDENT + (int)ui_cells(tag) + 1;
    int columns = ui_columns();

    ui_pad(TOOL_INDENT);
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

    unsigned char *spans = shell_spans(name, arg, strlen(arg));

    struct ui_wrap w = {0};
    w.budget = (size_t)budget;
    w.indent = indent;
    w.role = UI_RESET;
    w.max_rows = TOOL_CALL_ROWS;
    w.spans = spans;
    ui_wrap_paint(arg, &w);
    free(spans);
}

void view_cluster_forget(struct turnview *v)
{
    free(v->line);
    free(v->spans);
    v->line = NULL;
    v->spans = NULL;
    v->tool[0] = '\0';
    v->onscreen = 0;
}

/* Highlights the command that follows `prefix` bytes of chrome in `row`. */
static unsigned char *row_spans(const char *name, const char *row, size_t prefix)
{
    size_t len = strlen(row);
    if (!toolstyle_is_shell(name) || len <= prefix)
        return NULL;

    unsigned char *spans = malloc(len);
    if (!spans)
        return NULL;
    memset(spans, (unsigned char)UI_RESET, prefix);
    highlight_shell(row + prefix, len - prefix, spans + prefix);
    return spans;
}

static int cluster_budget(void)
{
    int budget = ui_columns() - TOOL_INDENT - 2;
    return budget < 8 ? 8 : budget;
}

// The whole row is kept; it is cut to the width when drawn.
void view_cluster_start(struct turnview *v, const char *name, const char *arg, int gap)
{
    char tag[64];
    tool_tag(name, tag, sizeof tag);

    char flat[4096];
    text_one_line(arg ? arg : "", flat, sizeof flat);

    char row[4096];
    snprintf(row, sizeof row, "%s %s", tag, flat);

    view_cluster_forget(v);
    snprintf(v->tool, sizeof v->tool, "%s", name);
    v->line = strdup(row);
    v->spans = v->line ? row_spans(name, row, strlen(tag) + 1) : NULL;
    v->gap = gap;
}

int view_cluster_extend(struct turnview *v, const char *name, const char *arg)
{
    if (!v->line || !v->after_collapse || strcmp(v->tool, name) != 0)
        return 0;
    if (!arg || !*arg)
        return 1;

    char flat[4096];
    text_one_line(arg, flat, sizeof flat);

    char row[4096];
    snprintf(row, sizeof row, "%s, %s", v->line, flat);
    if ((int)ui_cells(row) > cluster_budget())
        return 0;

    char *grown = strdup(row);
    if (!grown)
        return 0;

    unsigned char *spans = NULL;
    if (v->spans) {
        size_t kept = strlen(v->line);
        spans = row_spans(name, row, kept + 2);
        if (spans)
            memcpy(spans, v->spans, kept);
    }
    free(v->line);
    free(v->spans);
    v->line = grown;
    v->spans = spans;
    return 1;
}

static void cluster_paint(const char *line, const unsigned char *spans)
{
    size_t tag = strcspn(line, "]");
    if (line[tag])
        tag++;

    size_t len = strlen(line);
    size_t skip = 0;
    size_t fit = ui_wrap_row(line, len, (size_t)cluster_budget(), &skip, NULL);
    if (fit < tag)
        fit = tag;
    if (fit > len)
        fit = len;

    ui_pad(TOOL_INDENT);
    ui_esc(ui_style(UI_TOOL));
    ui_putn(line, tag);
    ui_esc(ui_style(UI_RESET));
    if (spans)
        ui_put_spans(line + tag, fit - tag, spans + tag, UI_RESET);
    else
        ui_putn(line + tag, fit - tag);
    if (fit < len)
        ui_put("…");
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

// One entry, amended: another call of the same tool changes what it says.
void view_cluster_paint(struct turnview *v)
{
    if (!v->line)
        return;

    size_t         len = strlen(v->line);
    unsigned char *spans = NULL;
    if (v->spans && (spans = malloc(len)))
        memcpy(spans, v->spans, len);

    struct keep *k = v->onscreen ? viewport_item_data(v->mark) : NULL;
    if (k) {
        free(k->a);
        free(k->spans);
        k->a = strdup(v->line);
        k->spans = spans;
        viewport_item_update(v->mark);
        return;
    }

    k = keep_new(KEEP_CLUSTER);
    if (!k) {
        free(spans);
        return;
    }
    k->a = strdup(v->line);
    k->spans = spans;
    k->gap = v->gap;
    v->mark = keep(k);
    v->onscreen = 1;
}

#define TOOL_PREVIEW_ROWS 3

static int preview_budget(void)
{
    int budget = ui_columns() - 6;
    return budget < 8 ? 8 : budget;
}

static int preview_line(const char *start, size_t n, int budget, enum ui_role role)
{
    char line[1024];
    if (n >= sizeof line)
        n = sizeof line - 1;
    memcpy(line, start, n);
    line[n] = '\0';

    char clipped[1024];
    text_one_line(line, clipped, sizeof clipped);
    if (!*clipped)
        return 0;

    size_t skip = 0;
    size_t fit = ui_wrap_row(clipped, strlen(clipped), (size_t)budget, &skip, NULL);
    ui_put("    ");
    ui_esc(ui_style(role));
    ui_putn(clipped, fit);
    if (clipped[fit])
        ui_put("…");
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
    return 1;
}

static void preview_elision(int lines)
{
    if (lines <= 0)
        return;
    ui_put("    ");
    ui_esc(ui_style(UI_DIM));
    ui_printf("+%d line%s", lines, lines == 1 ? "" : "s");
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

void view_tool_error(const char *text)
{
    if (!text || !*text)
        return;

    const char *head = NULL;
    size_t      head_n = 0;
    const char *tail[TOOL_PREVIEW_ROWS] = {0};
    size_t      tail_n[TOOL_PREVIEW_ROWS] = {0};
    int         total = 0;

    for (const char *p = text; *p;) {
        const char *nl = strchr(p, '\n');
        size_t      n = nl ? (size_t)(nl - p) : strlen(p);
        size_t      i = 0;
        while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r'))
            i++;
        if (i < n) {
            if (!total++) {
                head = p;
                head_n = n;
            } else {
                int slot = (total - 2) % TOOL_PREVIEW_ROWS;
                tail[slot] = p;
                tail_n[slot] = n;
            }
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    if (!total)
        return;

    int budget = preview_budget();
    preview_line(head, head_n, budget, UI_ERROR);

    int kept = total - 1 < TOOL_PREVIEW_ROWS ? total - 1 : TOOL_PREVIEW_ROWS;
    preview_elision(total - 1 - kept);
    for (int i = 0; i < kept; i++) {
        int slot = (total - 1 - kept + i) % TOOL_PREVIEW_ROWS;
        preview_line(tail[slot], tail_n[slot], budget, UI_ERROR);
    }
}

void view_tool_output(const char *text, enum ui_role role)
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
        text_one_line(line, clipped, sizeof clipped);
        if (*clipped) {
            size_t skip = 0;
            size_t fit = ui_wrap_row(clipped, strlen(clipped), (size_t)budget, &skip, NULL);
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

void view_free(struct turnview *v)
{
    free(v->line);
    free(v->spans);
    v->line = NULL;
    v->spans = NULL;
}
