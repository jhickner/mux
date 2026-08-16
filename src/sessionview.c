#include "sessionview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"

#define TOOL_INDENT 2

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

    struct ui_wrap w = {0};
    w.budget = (size_t)budget;
    w.indent = indent;
    w.role = UI_RESET;
    ui_wrap_paint(arg, &w);
}

void view_cluster_forget(struct turnview *v)
{
    free(v->line);
    v->line = NULL;
    v->tool[0] = '\0';
    v->onscreen = 0;
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

    int budget = cluster_budget() - (int)ui_cells(tag) - 1;
    if (budget < 8)
        budget = 8;

    size_t skip = 0;
    size_t fit = arg && *arg ? ui_wrap_row(arg, (size_t)budget, &skip) : 0;

    char row[4096];
    snprintf(row, sizeof row, "%s %.*s%s", tag, (int)fit, arg ? arg : "",
             arg && arg[fit] ? "…" : "");

    view_cluster_forget(v);
    snprintf(v->tool, sizeof v->tool, "%s", name);
    v->line = strdup(row);
    v->columns = ui_columns();
}

int view_cluster_extend(struct turnview *v, const char *name, const char *arg)
{
    if (!v->line || !v->after_collapse || strcmp(v->tool, name) != 0 ||
        v->columns != ui_columns())
        return 0;
    if (!arg || !*arg)
        return 1;

    char row[4096];
    snprintf(row, sizeof row, "%s, %s", v->line, arg);
    if ((int)ui_cells(row) > cluster_budget())
        return 0;

    char *grown = strdup(row);
    if (!grown)
        return 0;
    free(v->line);
    v->line = grown;
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
    ui_put(v->line + tag);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
    v->onscreen = 1;
}

#define TOOL_PREVIEW_ROWS 3

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

void view_free(struct turnview *v)
{
    free(v->line);
    v->line = NULL;
}
