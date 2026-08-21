#include "hud.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "gitinfo.h"
#include "session.h"
#include "tg.h"
#include "ui.h"
#include "viewport.h"
#include "text.h"
#include "workspace.h"

#define SEP    " \xe2\x80\xba "
#define BRANCH "\xee\x82\xa0"

struct seg {
    const char   *text;
    enum ui_role  role;
};

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

    // A second front end is attached: what is typed here is not the only way in,
    // and what the agent does is being relayed. Worth seeing at a glance.
    const char *chat = tg_label();
    char attached[128] = "";
    if (chat)
        snprintf(attached, sizeof attached, SEP "%s", chat);

    // Which of the window's sessions this is, when there is more than one.
    char sessions[32] = "";
    int count = workspace_count();
    if (count > 1)
        snprintf(sessions, sizeof sessions, SEP "session %d/%d",
                 workspace_index() + 1, count);

    struct seg segs[] = {
        {UI_BAR " ", UI_BRAND},
        {APP_NAME,   UI_BOLD},
        {tail,       UI_DIM},
        {sessions,   UI_ACCENT},
        {attached,   UI_OK},
    };
    return paint_row(segs, (int)(sizeof segs / sizeof *segs), cols);
}

static int row_location(const struct session *s, int cols)
{
    const char *dir = session_workdir(s);

    char path[1024];
    path_home_relative(dir, path, sizeof path);

    const struct gitinfo *g = gitinfo_get(dir);
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

void hud_print(const struct session *s)
{
    if (!s)
        return;
    int cols = ui_columns();
    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    row_identity(s, cols);
    row_location(s, cols);
    viewport_item_end();
    ui_flush();
}
