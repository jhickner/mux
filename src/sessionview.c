#include "sessionview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "highlight.h"
#include "text.h"
#include "toolstyle.h"
#include "vendor/cJSON.h"

#define TOOL_INDENT 2

#define COUNT(a) (sizeof (a) / sizeof *(a))

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

void view_cluster_start(struct turnview *v, const char *name, const char *arg)
{
    char tag[64];
    tool_tag(name, tag, sizeof tag);

    char flat[4096];
    text_one_line(arg ? arg : "", flat, sizeof flat);

    int budget = cluster_budget() - (int)ui_cells(tag) - 1;
    if (budget < 8)
        budget = 8;

    size_t skip = 0;
    size_t fit = *flat ? ui_wrap_row(flat, strlen(flat), (size_t)budget, &skip, NULL) : 0;

    char row[4096];
    snprintf(row, sizeof row, "%s %.*s%s", tag, (int)fit, flat, flat[fit] ? "…" : "");

    view_cluster_forget(v);
    snprintf(v->tool, sizeof v->tool, "%s", name);
    v->line = strdup(row);
    v->spans = v->line ? row_spans(name, row, strlen(tag) + 1) : NULL;
    v->columns = ui_columns();
}

int view_cluster_extend(struct turnview *v, const char *name, const char *arg)
{
    if (!v->line || !v->after_collapse || strcmp(v->tool, name) != 0 ||
        v->columns != ui_columns())
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

void view_cluster_paint(struct turnview *v)
{
    if (!v->line)
        return;
    size_t tag = strcspn(v->line, "]");
    if (v->line[tag])
        tag++;

    if (v->onscreen)
        ui_esc("\x1b[1A\r\x1b[K");
    ui_pad(TOOL_INDENT);
    ui_esc(ui_style(UI_TOOL));
    ui_putn(v->line, tag);
    ui_esc(ui_style(UI_RESET));
    if (v->spans)
        ui_put_spans(v->line + tag, strlen(v->line) - tag, v->spans + tag, UI_RESET);
    else
        ui_put(v->line + tag);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
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
