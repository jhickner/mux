
#ifndef SESSIONVIEW_H
#define SESSIONVIEW_H

#include "vendor/cJSON.h"

#include <stddef.h>

#include "ui.h"
#include "vendor/agents/backend.h"

struct turnview {
    char  tool[64];
    char *line;
    unsigned char *spans;
    int   gap;
    int   onscreen;
    unsigned mark;              /* the entry the cluster keeps amending */

    int   after_activity;
    int   after_tool;
    int   after_collapse;
};

void view_free(struct turnview *v);

void view_cluster_forget(struct turnview *v);

void view_tool_argument(const backend_event *ev, const char *cwd, char *out, size_t size);

int  view_tool_path(const char *input_json, const char *cwd, char *out, size_t size);

void view_activity(const char *marker, const char *text, enum ui_role role);

void view_tool_call(const char *name, const char *arg);

int  view_cluster_extend(struct turnview *v, const char *name, const char *arg);

void view_cluster_start(struct turnview *v, const char *name, const char *arg, int gap);

void view_cluster_paint(struct turnview *v);

void view_tool_output(const char *text, enum ui_role role);

void view_tool_error(const char *text);

// The same things as entries that redraw themselves. `gap` puts a blank row
// above, inside the entry.
void view_keep_activity(const char *marker, const char *text, enum ui_role role, int gap);

void view_keep_tool_call(const char *name, const char *arg, int gap);

void view_keep_output(const char *text, enum ui_role role, int error);

// Takes the patch.
void view_keep_diff(char *patch);

// Carried across a restart.
#define VIEW_KEEP_KIND "keep"
void view_keep_load(const cJSON *st);

#endif
