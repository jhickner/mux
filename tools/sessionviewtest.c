#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sessionview.h"
#include "text.h"
#include "ui.h"

static int fail(const char *what, const char *out)
{
    fprintf(stderr, "sessionviewtest: %s\n---\n%s---\n", what, out ? out : "");
    return 1;
}

static int lines_of(const char *s)
{
    int n = 0;
    for (const char *p = s; *p; p++)
        if (*p == '\n')
            n++;
    return n;
}

int main(void)
{
    char block[256];
    text_block("\n\n  import json\n  \nfor l in L:\n    print(l)\n\n\n", block, sizeof block);
    if (strcmp(block, "import json\nfor l in L:\n    print(l)") != 0)
        return fail("text_block did not preserve line structure", block);

    const backend_event tool = {.name = "Bash",
                                .input_json = "{\"command\":\"python - <<'EOF'\\n"
                                              "import json\\n"
                                              "d = json.load(open('x.json'))\\n"
                                              "EOF\"}"};
    char arg[4096];
    view_tool_argument(&tool, NULL, arg, sizeof arg);
    if (!strstr(arg, "\nimport json\n"))
        return fail("view_tool_argument flattened the command", arg);

    ui_capture_begin(80);
    view_tool_call("Bash", arg);
    char *out = ui_capture_end();
    if (!out || lines_of(out) != 4 || !strstr(out, "import json"))
        return fail("view_tool_call flattened the command", out);
    free(out);

    /* The collapsed cluster line stays one line even for a multi-line arg. */
    struct turnview v = {0};
    ui_capture_begin(200);
    view_cluster_start(&v, "Bash", arg, 0);
    view_cluster_paint(&v);
    out = ui_capture_end();
    if (!out || lines_of(out) != 1)
        return fail("cluster line spans rows", out);
    free(out);

    /* The whole row is kept: a narrow pane cuts it short when it draws it,
       rather than the row having been cut short when it was first printed. */
    ui_capture_begin(40);
    view_cluster_paint(&v);
    char *cut = ui_capture_end();
    ui_capture_begin(200);
    view_cluster_paint(&v);
    char *full = ui_capture_end();
    if (!cut || !full || lines_of(cut) != 1)
        return fail("a cluster row stays one row at any width", cut);
    if (!strstr(cut, "\xe2\x80\xa6"))
        return fail("a cluster row too wide for the pane is cut short", cut);
    if (strlen(full) <= strlen(cut))
        return fail("a cluster row is laid out for the width it is drawn at", full);
    free(cut);
    free(full);
    view_cluster_forget(&v);

    /* Errors keep the head line and the tail, where the exception lives. */
    ui_capture_begin(80);
    view_tool_error("failed: Exit code 1\n"
                    "Traceback (most recent call last):\n"
                    "  File \"<string>\", line 3, in <module>\n"
                    "    d = json.load(open('x.json'))\n"
                    "FileNotFoundError: 'x.json'\n");
    out = ui_capture_end();
    if (!out || !strstr(out, "failed: Exit code 1") || !strstr(out, "FileNotFoundError") ||
        !strstr(out, "+1 line"))
        return fail("view_tool_error dropped the exception", out);
    free(out);

    puts("sessionviewtest: all checks passed");
    return 0;
}
