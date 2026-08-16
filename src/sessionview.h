
#ifndef SESSIONVIEW_H
#define SESSIONVIEW_H

#include "ui.h"

/* How the current turn is being drawn. Consecutive identical tool calls are
   collapsed onto one line that is rewritten in place, so the view has to
   remember what it last painted and at what width. */
struct turnview {
    char  tool[64];   /* the collapsed cluster's tool name */
    char *line;       /* the collapsed cluster's text, or NULL */
    int   columns;    /* width the cluster was painted at */
    int   onscreen;   /* the cluster occupies the row above the cursor */

    int   after_activity;
    int   after_tool;
    int   after_collapse;
};

void view_free(struct turnview *v);

/* Ends any in-progress cluster; the next tool call starts a fresh one. */
void view_cluster_forget(struct turnview *v);

/* Marker plus wrapped text, indented under the tool column. */
void view_activity(const char *marker, const char *text, enum ui_role role);

/* Draws "[tool] arg". */
void view_tool_call(const char *name, const char *arg);

/* Adds arg to the current cluster when it is the same tool at the same width,
   and returns 1. Returns 0 when a new cluster is needed. */
int  view_cluster_extend(struct turnview *v, const char *name, const char *arg);

/* Begins a cluster for this tool and argument. */
void view_cluster_start(struct turnview *v, const char *name, const char *arg);

/* Repaints the cluster row, replacing the one already onscreen. */
void view_cluster_paint(struct turnview *v);

/* A few indented preview rows of tool output, then a "+N lines" tail. */
void view_tool_output(const char *text, enum ui_role role);

#endif
