#include "sessionswitch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cmd.h"
#include "handoff.h"
#include "hud.h"
#include "livelist.h"
#include "pick.h"
#include "scrollback.h"
#include "sessionload.h"
#include "session.h"
#include "text.h"
#include "ui.h"
#include "vendor/agents/backend.h"
#include "workspace.h"

#define KEY_CLOSE  0x18   /* ctrl-x */
#define KEY_NEW    0x0e   /* ctrl-n */

#define MAX_ROWS 128

enum row_kind {
    ROW_TAB,
    ROW_LIVE,
    ROW_NEW,
};

struct row {
    enum row_kind kind;
    int  at;            /* index into whichever list the kind names */
    char label[256];
    char detail[512];
};

static void relative_time(long then, char *out, size_t size)
{
    long secs = (long)time(NULL) - then;
    if (secs < 60)
        snprintf(out, size, "just now");
    else if (secs < 3600)
        snprintf(out, size, "%ldm ago", secs / 60);
    else if (secs < 86400)
        snprintf(out, size, "%ldh ago", secs / 3600);
    else
        snprintf(out, size, "%ldd ago", secs / 86400);
}

// The same trim session_model_short() does, for the rows whose session lives
// in another process and is only a set of strings here.
static const char *short_model(const char *backend, const char *model)
{
    if (!strcmp(backend, "claude") && !strncmp(model, "claude-", 7) && model[7])
        return model + 7;
    return model;
}

static const char *status_mark(const char *status)
{
    if (!strcmp(status, "working"))
        return "\xe2\x97\x8f";   /* a filled dot: a turn is running */
    if (!strcmp(status, "errored"))
        return "\xc3\x97";
    return " ";
}

static void tab_rows(struct row *rows, int *n)
{
    for (int i = 0; i < workspace_count() && *n < MAX_ROWS; i++) {
        struct session *s = workspace_at(i);
        const char *title = session_title(s);
        const char *status = workspace_status(s);
        struct row *r = &rows[(*n)++];
        r->kind = ROW_TAB;
        r->at = i;

        // The directory comes first in every row: it is the thing worth
        // seeing at a glance, and the one that survives a narrow window.
        char home[4096];
        path_home_relative(session_cwd(s), home, sizeof home);
        snprintf(r->label, sizeof r->label, "%s %s%s",
                 i == workspace_index() ? "\xe2\x96\xb8" : " ",
                 title && *title ? title : "untitled",
                 i == workspace_index() ? " (here)" : "");
        snprintf(r->detail, sizeof r->detail, "%s \xc2\xb7 %s %s %s",
                 home, session_backend(s),
                 session_model_short(s, session_model_label(s)),
                 status_mark(status));
    }
}

static void live_rows(struct row *rows, int *n, const struct live_session *live, int count)
{
    for (int i = 0; i < count && *n < MAX_ROWS; i++) {
        const struct live_session *v = &live[i];
        if (v->mine || !v->id[0])
            continue;

        char home[4096], when[32];
        path_home_relative(v->cwd, home, sizeof home);
        relative_time(v->ts, when, sizeof when);

        struct row *r = &rows[(*n)++];
        r->kind = ROW_LIVE;
        r->at = i;
        snprintf(r->label, sizeof r->label, "\xe2\x87\x84 %s",
                 v->title[0] ? v->title : "untitled");
        snprintf(r->detail, sizeof r->detail,
                 "%s \xc2\xb7 %s %s \xc2\xb7 another window \xc2\xb7 %s %s",
                 home, v->backend,
                 short_model(v->backend, v->label[0] ? v->label : v->model), when,
                 status_mark(v->status));
    }
}

// A live record holds everything a window needs to open the same conversation
// once the window that had it lets go.
static void yank(const struct live_session *v)
{
    ui_bar(ui_style(UI_DIM), "asking %s for it\xe2\x80\xa6", v->pane[0] ? v->pane : "the other window");
    ui_put("\n");
    ui_flush();

    char screen[4400];
    if (!handoff_ask(v->pid, v->id, screen, sizeof screen)) {
        ui_error("that window would not let go of it");
        ui_put("\n");
        ui_flush();
        return;
    }

    int at = workspace_spawn(v->backend, v->model, v->effort, v->cwd, v->id);
    if (at < 0) {
        unlink(screen);
        ui_error("could not open it here");
        ui_put("\n");
        ui_flush();
        return;
    }

    // The screen it had travels with it, so the conversation reads as one
    // thing rather than starting again at a bare resume. A window that had
    // nothing to send — a session it had only just resumed itself — leaves an
    // empty file, and the conversation comes back off the disk instead.
    struct stat st;
    if (stat(screen, &st) == 0 && st.st_size > 0)
        scrollback_restore(screen);
    else
        sessionload_into(workspace_current());
    unlink(screen);
    ui_bar(ui_style(UI_DIM), "yanked \xc2\xb7 %s", v->title[0] ? v->title : v->backend);
    ui_put("\n");
    ui_flush();
}

static void open_new(void)
{
    const char *const *names = backend_names();
    struct pick_item choices[8];
    int count = 0;
    for (const char *const *p = names; *p && count < 8; p++)
        choices[count++] = (struct pick_item){*p, NULL};

    int which = pick_run("a new session in which backend", choices, count, 0);
    if (which < 0)
        return;
    const char *backend = choices[which].label;

    int models = 0;
    const struct pick_item *list = cmd_model_choices(backend, &models);
    const char *model = NULL;
    if (models > 1) {
        int pickedm = pick_run_filter("which model", list, models, 0);
        if (pickedm < 0)
            return;
        model = list[pickedm].label;
    }

    struct session *here = workspace_current();
    if (workspace_spawn(backend, model, NULL, here ? session_cwd(here) : NULL, NULL) < 0) {
        ui_error("could not start the %s CLI", backend);
        ui_put("\n");
        ui_flush();
        return;
    }
    hud_print(workspace_current());
    ui_flush();
}

void sessionswitch_run(void)
{
    struct live_session *live = NULL;
    int nlive = livelist_load(&live);

    struct row *rows = calloc(MAX_ROWS, sizeof *rows);
    if (!rows) {
        free(live);
        return;
    }

    int n = 0;
    tab_rows(rows, &n);
    live_rows(rows, &n, live, nlive);
    if (n < MAX_ROWS) {
        struct row *r = &rows[n++];
        r->kind = ROW_NEW;
        snprintf(r->label, sizeof r->label, "+ new session");
        snprintf(r->detail, sizeof r->detail, "pick a backend and model");
    }

    struct pick_item *items = calloc((size_t)n, sizeof *items);
    if (!items) {
        free(rows);
        free(live);
        return;
    }
    for (int i = 0; i < n; i++) {
        items[i].label = rows[i].label;
        items[i].detail = rows[i].detail;
    }

    char shortcuts[3] = {KEY_CLOSE, KEY_NEW, 0};
    int pressed = 0;
    int picked = pick_run_ex("sessions \xc2\xb7 ^n new \xc2\xb7 ^x close", items, n,
                             workspace_index(), shortcuts, &pressed);

    struct row chosen = {0};
    if (picked >= 0)
        chosen = rows[picked];

    free(items);
    free(rows);

    if (picked < 0) {
        free(live);
        return;
    }

    if (pressed == KEY_NEW) {
        free(live);
        open_new();
        return;
    }

    if (pressed == KEY_CLOSE) {
        // Only this window's own tabs are ours to close.
        if (chosen.kind == ROW_TAB && workspace_count() > 1) {
            workspace_close(chosen.at);
        } else if (chosen.kind == ROW_TAB) {
            ui_note("that is the only session here");
            ui_put("\n");
            ui_flush();
        }
        free(live);
        return;
    }

    switch (chosen.kind) {
    case ROW_TAB:
        workspace_show(chosen.at);
        break;
    case ROW_LIVE:
        yank(&live[chosen.at]);
        break;
    case ROW_NEW:
        free(live);
        open_new();
        return;
    }

    free(live);
}

/* --- the other side of a yank -------------------------------------------- */

static int gave_last;

int sessionswitch_gave_last(void) { return gave_last; }

void sessionswitch_serve_request(void)
{
    char id[128];
    if (!handoff_take_request(id, sizeof id))
        return;

    int at = workspace_find_id(id);
    if (at < 0) {
        handoff_refuse(id);
        return;
    }

    char screen[4400];
    if (handoff_screen_path(id, screen, sizeof screen))
        workspace_dump(at, screen);

    // The agent goes before the announcement: two processes must never hold
    // the same conversation at once.
    int left = workspace_close(at);
    handoff_publish(id);

    if (!left) {
        gave_last = 1;
        return;
    }
    ui_bar(ui_style(UI_DIM), "handed a session to another window");
    ui_put("\n");
    ui_flush();
}
