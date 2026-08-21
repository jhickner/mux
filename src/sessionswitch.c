#include "sessionswitch.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ask.h"
#include "cmd.h"
#include "handoff.h"
#include "hud.h"
#include "livelist.h"
#include "pick.h"
#include "scrollback.h"
#include "sessionload.h"
#include "session.h"
#include "status.h"
#include "text.h"
#include "title.h"
#include "ui.h"
#include "vendor/agents/backend.h"
#include "workspace.h"

#define KEY_CLOSE  0x18   /* ctrl-x */
#define KEY_NEW    0x0e   /* ctrl-n */
#define KEY_GO     0x07   /* ctrl-g */
#define KEY_RENAME 0x12   /* ctrl-r */
#define KEY_ASK    0x10   /* ctrl-p */

#define MAX_ROWS 128

enum row_kind {
    ROW_TAB,
    ROW_LIVE,
    ROW_NEW,
    ROW_HEAD,
};

struct row {
    enum row_kind kind;
    int  at;            /* index into whichever list the kind names */
    int  spin;          /* a turn is running: the status column turns */
    char mark[4];       /* what the status column says when it does not */
    char id[128];
    char cwd[512];      /* what the row is grouped under */
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

// The same states the tmux tabs show: a spinner while a turn runs, a mark
// when one ended badly, nothing when there is nothing to say.
static void row_status(struct row *r, const char *status)
{
    r->spin = status && !strcmp(status, "working");
    snprintf(r->mark, sizeof r->mark, "%s",
             status && !strcmp(status, "errored") ? "e" : "");
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

        // The directory is the row's group, printed once above it.
        path_home_relative(session_cwd(s), r->cwd, sizeof r->cwd);
        snprintf(r->label, sizeof r->label, "%s %s%s",
                 i == workspace_index() ? "\xe2\x96\xb8" : " ",
                 title && *title ? title : "untitled",
                 i == workspace_index() ? " (here)" : "");
        snprintf(r->id, sizeof r->id, "%s", session_id(s) ? session_id(s) : "");
        snprintf(r->detail, sizeof r->detail, "%s %s",
                 session_backend(s),
                 session_model_short(s, session_model_label(s)));
        row_status(r, status);
    }
}

// Which window each pane sits in, asked once per listing. A record published
// by a build that did not know its window, or before tmux told it, still says
// where it is: the pane id it carries is enough to look up.
#define MAX_PANES 256

static struct {
    char pane[16];
    char window[16];
    char name[64];
} panes[MAX_PANES];
static int npanes;

static void panes_load(void)
{
    npanes = 0;
    if (!getenv("TMUX"))
        return;
    // A window name can hold spaces, so it comes last and takes the rest.
    FILE *p = popen("tmux list-panes -a -F "
                    "'#{pane_id} #{window_id} #{window_index}:#{window_name}' "
                    "2>/dev/null", "r");
    if (!p)
        return;
    char line[256];
    while (npanes < MAX_PANES && fgets(line, sizeof line, p)) {
        char pane[16], window[16];
        int  at = 0;
        if (sscanf(line, "%15s %15s %n", pane, window, &at) < 2 || !at)
            continue;
        line[strcspn(line, "\n")] = '\0';
        snprintf(panes[npanes].pane, sizeof panes[npanes].pane, "%s", pane);
        snprintf(panes[npanes].window, sizeof panes[npanes].window, "%s", window);
        snprintf(panes[npanes].name, sizeof panes[npanes].name, "%s", line + at);
        npanes++;
    }
    pclose(p);
}

static int pane_at(const char *pane)
{
    for (int i = 0; i < npanes; i++)
        if (!strcmp(panes[i].pane, pane))
            return i;
    return -1;
}

// What the row says about a session another window is holding. Redone while
// the list is open, so a turn starting or ending over there shows here.
static void fill_live(struct row *r, const struct live_session *v)
{
    char when[32];
    relative_time(v->ts, when, sizeof when);

    // Under tmux, a session in this same window is nearer than one in a
    // window elsewhere: both are taken the same way, but one is in sight.
    const char *here = livelist_tmux_window();
    int at = v->pane[0] ? pane_at(v->pane) : -1;
    // What tmux says now beats what the record said when it was written:
    // a window can be renamed, and a pane moved, under a session.
    const char *window = at >= 0 ? panes[at].window : (v->window[0] ? v->window : NULL);
    const char *wname = at >= 0 ? panes[at].name : (v->wname[0] ? v->wname : NULL);
    int in_tmux = here[0] != '\0';
    int near = in_tmux && window && !strcmp(window, here);
    // The window it is in, unless that is this one: a row with nothing to
    // say about where it lives is here.
    char where[96] = "";
    if (in_tmux && !near && wname && *wname)
        snprintf(where, sizeof where, "%s \xc2\xb7 ", wname);

    // The arrow says another mux is holding this one: choosing it takes it
    // over here, where the rows without an arrow only switch.
    snprintf(r->label, sizeof r->label, "%s %s",
             near ? "\xe2\x87\xa2" : "\xe2\x87\x84",
             v->title[0] ? v->title : "untitled");
    snprintf(r->detail, sizeof r->detail, "%s %s \xc2\xb7 %s%s",
             v->backend,
             short_model(v->backend, v->label[0] ? v->label : v->model),
             where, when);
    row_status(r, v->status);
}

static void live_rows(struct row *rows, int *n, const struct live_session *live, int count)
{
    for (int i = 0; i < count && *n < MAX_ROWS; i++) {
        const struct live_session *v = &live[i];
        if (v->mine || !v->id[0])
            continue;

        struct row *r = &rows[(*n)++];
        r->kind = ROW_LIVE;
        r->at = i;
        snprintf(r->id, sizeof r->id, "%s", v->id);
        path_home_relative(v->cwd, r->cwd, sizeof r->cwd);
        fill_live(r, v);
    }
}

// The rows again, gathered under their directory: this window's own directory
// first, the rest by name. Returns how many rows the grouped list holds.
static int group_rows(const struct row *in, int n, struct row *out,
                      unsigned char *heading, int max)
{
    struct session *here = workspace_current();
    char mine[512] = "";
    if (here)
        path_home_relative(session_cwd(here), mine, sizeof mine);

    char used[MAX_ROWS] = {0};
    int m = 0;

    for (;;) {
        const char *group = NULL;
        for (int i = 0; i < n; i++) {
            if (used[i])
                continue;
            if (mine[0] && !strcmp(in[i].cwd, mine)) {
                group = mine;
                break;
            }
            if (!group || strcmp(in[i].cwd, group) < 0)
                group = in[i].cwd;
        }
        if (!group)
            break;

        if (m < max) {
            out[m].kind = ROW_HEAD;
            snprintf(out[m].label, sizeof out[m].label, "%s", group);
            heading[m] = 1;
            m++;
        }
        for (int i = 0; i < n; i++) {
            if (used[i] || strcmp(in[i].cwd, group) != 0)
                continue;
            used[i] = 1;
            if (m >= max)
                continue;
            out[m] = in[i];
            heading[m] = 0;
            m++;
        }
    }
    return m;
}

static int tmux_do(const char *verb, const char *target)
{
    pid_t pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
            if (null > STDERR_FILENO)
                close(null);
        }
        char *argv[] = {"tmux", (char *)verb, "-t", (char *)target, NULL};
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Leave the session where it is and put tmux's eyes on it instead.
static void jump(const struct live_session *v)
{
    if (!getenv("TMUX") || !v->pane[0]) {
        ui_error("that one is not in a tmux pane");
        ui_put("\n");
        ui_flush();
        return;
    }
    // The window first: selecting the pane alone would not leave this one.
    if (!tmux_do("select-window", v->pane) || !tmux_do("select-pane", v->pane)) {
        ui_error("tmux would not go there");
        ui_put("\n");
        ui_flush();
    }
}

// A live record holds everything a window needs to open the same conversation
// once the window that had it lets go.
// A window at its prompt answers in a moment; one that is off running a
// command of its own only answers when it comes back, so the wait says so.
static void waiting(int waited_ms, void *ud)
{
    int *said = ud;
    if (waited_ms < 1000 || *said)
        return;
    *said = 1;
    ui_bar(ui_style(UI_DIM), "it has not answered yet \xc2\xb7 waiting\xe2\x80\xa6");
    ui_put("\n");
    ui_flush();
}

static void yank(const struct live_session *v)
{
    ui_bar(ui_style(UI_DIM), "asking %s for it\xe2\x80\xa6", v->pane[0] ? v->pane : "the other window");
    ui_put("\n");
    ui_flush();

    char screen[4400];
    int said = 0;
    if (!handoff_ask(v->pid, v->id, screen, sizeof screen, waiting, &said)) {
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

// A new session started from the list and left to run: the prompt is typed
// here, what it runs as is taken from the row it was started off, and the
// window stays where it was rather than following it in.
static void ask_new(const struct row *r, const struct live_session *live)
{
    const char *backend = NULL, *model = NULL, *cwd = NULL;
    if (r->kind == ROW_TAB) {
        const struct session *s = workspace_at(r->at);
        if (s) {
            backend = session_backend(s);
            model = session_model_label(s);
            cwd = session_cwd(s);
        }
    } else if (r->kind == ROW_LIVE && r->at >= 0) {
        const struct live_session *v = &live[r->at];
        backend = v->backend;
        model = v->model;
        cwd = v->cwd;
    }
    if (!backend) {
        const struct session *here = workspace_current();
        if (!here)
            return;
        backend = session_backend(here);
        model = session_model_label(here);
        cwd = session_cwd(here);
    }

    char *line = ask_run("a new session, with this to get on with", NULL);
    if (!line)
        return;
    if (!*line) {
        free(line);
        return;
    }

    int was = workspace_index();
    int at = workspace_spawn(backend, model, NULL, cwd, NULL);
    if (at < 0) {
        ui_error("could not start the %s CLI", backend);
        ui_put("\n");
        ui_flush();
        free(line);
        return;
    }
    // Back to what the window was showing before the turn is sent, so the new
    // session's prompt does not take the sticky line off the tab in front.
    workspace_show(was);
    workspace_send(at, line, NULL);
    free(line);
}

// A session another window is holding is renamed through the shared title
// file: that window reads the name back the next time it publishes itself.
static void rename_row(const struct row *r, struct live_session *live)
{
    struct session *tab = r->kind == ROW_TAB ? workspace_at(r->at) : NULL;
    struct live_session *v = r->kind == ROW_LIVE ? &live[r->at] : NULL;
    if (!tab && !v)
        return;

    const char *was = tab ? session_title(tab) : (v->title[0] ? v->title : NULL);
    char *name = ask_run("rename this session", was);
    if (!name)
        return;

    int ok = tab ? session_rename(tab, name) : title_set(v->id, name);
    if (ok && v)
        snprintf(v->title, sizeof v->title, "%s", name);
    // A rename takes the name for the session it was for; the note on screen
    // belongs to whichever session this window is showing.
    if (ok && tab && tab != workspace_current())
        status_set_note(session_title(workspace_current()));
    if (!ok) {
        ui_error("could not use that name");
        ui_put("\n");
        ui_flush();
    }
    free(name);
}

// What the open list needs to keep saying the truth: the rows, the columns
// pick draws them with, and the records they were built from.
struct listing {
    struct row          *rows;
    int                  n;
    unsigned char       *spin;
    const char         **marks;
    struct live_session **live;
    int                 *nlive;
    double               read_at;
    unsigned long        sig;
};

static void sync_columns(struct listing *l)
{
    for (int i = 0; i < l->n; i++) {
        l->spin[i] = (unsigned char)l->rows[i].spin;
        l->marks[i] = l->rows[i].mark;
    }
}

// What the rows are showing, so a re-read that found nothing new does not
// cost a repaint.
static unsigned long listing_sig(const struct listing *l)
{
    unsigned long h = 5381;
    for (int i = 0; i < l->n; i++) {
        const struct row *r = &l->rows[i];
        const char *parts[] = {r->label, r->detail, r->mark};
        for (size_t p = 0; p < sizeof parts / sizeof *parts; p++)
            for (const char *c = parts[p]; c && *c; c++)
                h = h * 33 + (unsigned char)*c;
        h = h * 33 + (unsigned long)(r->spin + 1);
    }
    return h;
}

// A few times a second the records are read again, so a turn starting in
// another window reaches this list without closing it.
static int relist(void *ud)
{
    struct listing *l = ud;
    double now = now_seconds();
    if (l->read_at > 0 && now - l->read_at < 0.45)
        return 0;
    l->read_at = now;

    // The window is parked in this list, so its own turns only move on if the
    // list pumps them: without this a tab that finishes underneath stays as it
    // was when the list opened.
    workspace_pump_quiet();

    struct live_session *fresh = NULL;
    int nfresh = livelist_load(&fresh);
    free(*l->live);
    *l->live = fresh;
    *l->nlive = nfresh;

    for (int i = 0; i < l->n; i++) {
        struct row *r = &l->rows[i];
        if (r->kind == ROW_TAB) {
            struct session *s = r->at < workspace_count() ? workspace_at(r->at) : NULL;
            if (s)
                row_status(r, workspace_status(s));
            continue;
        }
        if (r->kind != ROW_LIVE)
            continue;
        // The row keeps its place; which record it names may have moved.
        r->at = -1;
        for (int j = 0; j < nfresh; j++)
            if (!strcmp(fresh[j].id, r->id)) {
                r->at = j;
                break;
            }
        if (r->at >= 0) {
            char label[sizeof r->label];
            snprintf(label, sizeof label, "%s", r->label);
            fill_live(r, &fresh[r->at]);
            // The name is what the query filters on, so it is left alone
            // while the list is open.
            snprintf(r->label, sizeof r->label, "%s", label);
        } else {
            // Gone while the list was open: nothing to take any more.
            r->spin = 0;
            r->mark[0] = '\0';
        }
    }
    sync_columns(l);

    unsigned long sig = listing_sig(l);
    if (sig == l->sig)
        return 0;
    l->sig = sig;
    return 1;
}

// Returns nonzero when the list should be shown again: a rename leaves the
// window where it was.
static int switch_once(void)
{
    struct live_session *live = NULL;
    int nlive = livelist_load(&live);

    struct row *found = calloc(MAX_ROWS, sizeof *found);
    struct row *rows = calloc(MAX_ROWS, sizeof *rows);
    unsigned char *heading = calloc(MAX_ROWS, 1);
    unsigned char *spin = calloc(MAX_ROWS, 1);
    const char **marks = calloc(MAX_ROWS, sizeof *marks);
    if (!found || !rows || !heading || !spin || !marks) {
        free(found);
        free(rows);
        free(heading);
        free(spin);
        free(marks);
        free(live);
        return 0;
    }

    int nfound = 0;
    panes_load();
    tab_rows(found, &nfound);
    live_rows(found, &nfound, live, nlive);

    int n = group_rows(found, nfound, rows, heading, MAX_ROWS);
    free(found);

    int initial = 0;
    for (int i = 0; i < n; i++)
        if (rows[i].kind == ROW_TAB && rows[i].at == workspace_index())
            initial = i;

    if (n < MAX_ROWS) {
        struct row *r = &rows[n];
        r->kind = ROW_NEW;
        snprintf(r->label, sizeof r->label, "+ new session");
        snprintf(r->detail, sizeof r->detail, "pick a backend and model");
        heading[n++] = 0;
    }

    struct pick_item *items = calloc((size_t)n, sizeof *items);
    if (!items) {
        free(rows);
        free(heading);
        free(spin);
        free(marks);
        free(live);
        return 0;
    }
    for (int i = 0; i < n; i++) {
        items[i].label = rows[i].label;
        items[i].detail = rows[i].detail;
    }

    char shortcuts[8] = {KEY_CLOSE, KEY_NEW, KEY_ASK, KEY_GO, KEY_RENAME, '\n',
                         PICK_KEY_RIGHT, 0};
    int pressed = 0;
    // Going to a session leaves this window for the one holding it, which
    // only tmux can do.
    char title[256];
    snprintf(title, sizeof title,
             "sessions \xc2\xb7 enter: bring here%s \xc2\xb7 ^n: new "
             "\xc2\xb7 ^p: new + prompt \xc2\xb7 ^r: rename \xc2\xb7 ^x: close",
             livelist_tmux_window()[0] ? " \xc2\xb7 shift-enter: go there" : "");
    struct listing listing = {rows, n, spin, marks, &live, &nlive, 0, 0};
    sync_columns(&listing);
    listing.sig = listing_sig(&listing);
    struct pick_live shown = {heading, spin, marks, relist, &listing};
    int picked = pick_run_live(title, items, n, initial, &shown, shortcuts, &pressed);

    struct row chosen = {0};
    if (picked >= 0)
        chosen = rows[picked];

    // Right is the way into the row: a session of this window's is switched
    // to, and one another window is holding is gone to, where it already is.
    if (pressed == PICK_KEY_RIGHT)
        pressed = chosen.kind == ROW_LIVE ? KEY_GO : 0;

    free(items);
    free(rows);
    free(heading);
    free(spin);
    free(marks);

    if (picked < 0) {
        free(live);
        return 0;
    }

    // A row whose session let go while the list was open has nothing behind
    // it any more.
    if (chosen.kind == ROW_LIVE && chosen.at < 0) {
        ui_note("that one is gone");
        ui_put("\n");
        ui_flush();
        free(live);
        return 0;
    }

    if (pressed == KEY_RENAME) {
        rename_row(&chosen, live);
        free(live);
        return 1;
    }

    if (pressed == KEY_NEW) {
        free(live);
        open_new();
        return 0;
    }

    // Started and left running: the list is what the window comes back to.
    if (pressed == KEY_ASK) {
        ask_new(&chosen, live);
        free(live);
        return 1;
    }

    if (pressed == KEY_GO || pressed == '\n') {
        if (chosen.kind == ROW_LIVE)
            jump(&live[chosen.at]);
        else if (chosen.kind == ROW_TAB)
            workspace_show(chosen.at);
        free(live);
        return 0;
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
        return 0;
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
        return 0;
    case ROW_HEAD:
        break;
    }

    free(live);
    return 0;
}

void sessionswitch_run(void)
{
    while (switch_once())
        ;
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
