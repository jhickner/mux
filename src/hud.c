#include "hud.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "gitinfo.h"
#include "session.h"
#include "ui.h"
#include "text.h"

#define SEP    " \xe2\x80\xba "
#define BRANCH "\xee\x82\xa0"

struct seg {
    const char   *text;
    enum ui_role  role;
};

/* Returns the row's painted width in cells. */
static int paint_row(const struct seg *segs, int count, int cols)
{
    size_t budget = (size_t)(cols > 1 ? cols - 1 : 1);
    size_t room = budget;
    ui_esc("\x1b[K");
    for (int i = 0; i < count && budget > 0; i++) {
        if (!segs[i].text || !*segs[i].text)
            continue;
        size_t bytes = ui_fit_bytes(segs[i].text, budget);
        if (bytes == 0)
            break;
        ui_esc(ui_style(segs[i].role));
        ui_putn(segs[i].text, bytes);
        budget -= ui_cells_n(segs[i].text, bytes);
    }
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
    return (int)(room - budget);
}

static int row_identity(const struct session *s, int cols)
{
    const char *backend = session_backend(s);
    const char *model = session_model_short(s, session_model_label(s));
    const char *effort = session_effort_label(s);

    char tail[256];
    snprintf(tail, sizeof tail, SEP "%s" SEP "%s%s%s", backend, model,
             effort ? SEP : "", effort ? effort : "");

    struct seg segs[] = {
        {UI_BAR " ", UI_BRAND},
        {APP_NAME,   UI_BOLD},
        {tail,       UI_DIM},
    };
    return paint_row(segs, (int)(sizeof segs / sizeof *segs), cols);
}

static int row_location(const struct session *s, int cols)
{
    char path[1024];
    path_home_relative(session_cwd(s), path, sizeof path);

    const struct gitinfo *g = gitinfo_get(session_cwd(s));
    char where[256] = "", added[32] = "", removed[32] = "", flags[8] = "", ctx[24] = "";
    if (g->repo)
        snprintf(where, sizeof where, " on " BRANCH " %s%s%s%s",
                 g->branch[0] ? g->branch : "detached",
                 g->sha[0] ? " (" : "", g->sha, g->sha[0] ? ")" : "");
    if (g->added)
        snprintf(added, sizeof added, " +%ld", g->added);
    if (g->removed)
        snprintf(removed, sizeof removed, " -%ld", g->removed);

    if (g->dirty || g->untracked)
        snprintf(flags, sizeof flags, " [%s%s]", g->dirty ? "!" : "",
                 g->untracked ? "?" : "");

    int percent = session_context_percent(s);
    if (percent >= 0)
        snprintf(ctx, sizeof ctx, " \xc2\xb7 %d%%", percent);

    struct seg segs[] = {
        {UI_BAR " ",              UI_BRAND},
        {path,                    UI_CHROME},
        {where,                   UI_DIM},
        {added,                   UI_OK},
        {removed,                 UI_ERROR},
        {flags,                   UI_ERROR},
        {ctx,                     UI_DIM},
    };
    return paint_row(segs, (int)(sizeof segs / sizeof *segs), cols);
}

static void note_width(int *widths, int max, int row, int width)
{
    if (widths && row < max)
        widths[row] = width;
}

int hud_paint_busy(void *ud, int cols, int *widths, int max)
{
    const struct session *s = ud;
    if (!s)
        return 0;
    note_width(widths, max, 0, row_identity(s, cols));
    note_width(widths, max, 1, row_location(s, cols));

    ui_esc("\x1b[K");
    ui_put("\n");
    note_width(widths, max, 2, 0);
    return 3;
}

int hud_paint_idle(void *ud, int cols, int *widths, int max)
{
    const struct session *s = ud;
    int rows = hud_paint_busy(ud, cols, widths, max);
    if (!rows)
        return 0;

    const char *footer = session_footer(s);
    if (footer) {
        struct seg segs[] = {{footer, UI_DIM}};
        note_width(widths, max, rows, paint_row(segs, 1, cols));
        rows++;
    }
    return rows;
}
