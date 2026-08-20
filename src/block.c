#include "block.h"

#include <stdlib.h>
#include <string.h>

#include "tty.h"
#include "ui.h"
#include "viewport.h"

// The chrome under the transcript: renders into a buffer, splits it into rows,
// hands them to the viewport. Nothing here touches the screen.

#define ROWS_MAX 128

static int row_edit = -1;
static int have;

static int split(char *body, char **line, int max)
{
    int   n = 0;
    char *s = body;
    while (n < max) {
        char *nl = strchr(s, '\n');
        if (nl)
            *nl = '\0';
        size_t l = strlen(s);
        while (l && s[l - 1] == '\r')
            s[--l] = '\0';
        line[n++] = s;
        if (!nl || !nl[1])
            break;
        s = nl + 1;
    }
    return n;
}

void block_begin(void)
{
    row_edit = -1;
    ui_sink_begin();
}

void block_end(int caret_row, int caret_col)
{
    char *body = ui_sink_end();
    if (!body) {
        block_clear();
        return;
    }

    char *line[ROWS_MAX];
    int   n = split(body, line, ROWS_MAX);

    int limit = tty_rows() - 1;
    if (n > limit)
        n = limit > 0 ? limit : 1;

    viewport_chrome(line, n, caret_row, caret_col);
    have = n > 0;
    viewport_paint();
    free(body);
}

int block_have(void) { return have; }

void block_row_begin(int row)
{
    row_edit = row;
    ui_sink_begin();
}

// One chrome row in place: saves re-rendering the rest, not the paint.
void block_row_end(void)
{
    char *body = ui_sink_end();
    int   at = row_edit;
    row_edit = -1;
    if (!body)
        return;

    char *line[ROWS_MAX];
    int   n = split(body, line, ROWS_MAX);
    if (at >= 0 && n > 0)
        viewport_chrome_row(at, line[0]);
    viewport_paint();
    free(body);
}

void block_clear(void)
{
    have = 0;
    viewport_chrome_clear();
    viewport_paint();
}

// The rows named here stop being chrome and become transcript.
void block_keep(int keep)
{
    viewport_chrome_keep(keep);
    have = 0;
    viewport_paint();
}

void block_forget(void)
{
    have = 0;
    viewport_chrome_clear();
}

void block_cleared(void)
{
    have = 0;
    viewport_chrome_clear();
    viewport_clear();
}
