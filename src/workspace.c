#include "workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "block.h"
#include "chrome.h"
#include "cmd.h"
#include "gitinfo.h"
#include "prompt.h"
#include "session.h"
#include "settings.h"
#include "status.h"
#include "tg.h"
#include "ui.h"
#include "viewport.h"

#define PENDING_MAX 8

// A line waiting behind the turn that was running when it was typed.
struct pending {
    char *line;
    char *shown;
};

struct tab {
    struct session        *s;
    struct viewport_state *screen;   /* empty while this tab is the one on screen */
    struct pending         pending[PENDING_MAX];
    int                    npending;
    char                  *sticky;   /* the prompt this tab is showing */
    int                    finished; /* a turn ended; what follows it is owed */
};

static struct tab tabs[WORKSPACE_MAX];
static int        ntabs;
static int        cur;
static int        safe;
static void     (*on_finish)(struct session *s);

static void follow(const struct session *s);

void workspace_on_finish(void (*fn)(struct session *s))
{
    on_finish = fn;
}

// The spinner belongs to the tab in front: a turn running behind it is shown
// by the tab strip instead.
static void spin_follow(void)
{
    static const struct session *spinning;
    const struct session *want = ntabs && session_turn_running(tabs[cur].s)
                                 ? tabs[cur].s : NULL;
    if (want == spinning)
        return;
    // A swap between two running tabs is still a swap: the block on the old
    // screen goes, and the new one is drawn where its own turn started.
    if (spinning)
        status_end();
    spinning = want;
    if (want) {
        session_spin_word(want);
        status_begin_at(session_turn_elapsed(want));
    }
}

int workspace_count(void) { return ntabs; }
int workspace_index(void) { return cur; }

struct session *workspace_current(void)
{
    return ntabs ? tabs[cur].s : NULL;
}

struct session *workspace_at(int index)
{
    return index >= 0 && index < ntabs ? tabs[index].s : NULL;
}

int workspace_index_of(const struct session *s)
{
    for (int i = 0; i < ntabs; i++)
        if (tabs[i].s == s)
            return i;
    return -1;
}

int workspace_begin(struct session *first, int safe_mode)
{
    safe = safe_mode;
    ntabs = 0;
    cur = 0;
    if (workspace_open(first) != 0)
        return 0;
    follow(first);
    return 1;
}

void workspace_end(void)
{
    for (int i = 0; i < ntabs; i++) {
        if (i != cur)
            viewport_state_free(tabs[i].screen);
        session_free(tabs[i].s);
        for (int j = 0; j < tabs[i].npending; j++) {
            free(tabs[i].pending[j].line);
            free(tabs[i].pending[j].shown);
        }
        free(tabs[i].sticky);
    }
    memset(tabs, 0, sizeof tabs);
    ntabs = 0;
}

int workspace_open(struct session *s)
{
    if (!s || ntabs >= WORKSPACE_MAX)
        return -1;
    struct viewport_state *screen = viewport_state_new();
    if (!screen)
        return -1;

    int at = ntabs++;
    memset(&tabs[at], 0, sizeof tabs[at]);
    tabs[at].s = s;
    tabs[at].screen = screen;
    if (at != cur)
        workspace_show(at);
    return at;
}

int workspace_spawn(const char *backend, const char *model, const char *effort,
                    const char *cwd, const char *id)
{
    if (ntabs >= WORKSPACE_MAX)
        return -1;
    if (!model || !strcmp(model, "default"))
        model = session_saved_model(backend);
    if (!effort || !strcmp(effort, "default"))
        effort = session_saved_effort(backend);

    struct session *s = session_new(backend, cwd, model, effort);
    if (!s)
        return -1;

    session_set_customizations(s, !safe);
    session_set_thinking(s, settings_get_int(SETTING_THINKING, 1));
    session_set_compact(s, settings_get_int(SETTING_COMPACT, 0));
    session_set_permission(s, session_permission_name(
        settings_get_int(SETTING_PERMISSION, session_permission_default())));
    session_adopt_id(s, id);

    if (!session_start(s)) {
        session_free(s);
        return -1;
    }
    int at = workspace_open(s);
    if (at < 0)
        session_free(s);
    return at;
}

// Everything outside the session that is about where it is: the window follows
// the tab it is showing, the way /cd makes it follow the session.
static void follow(const struct session *s)
{
    const char *dir = session_cwd(s);
    if (dir && *dir)
        (void)chdir(dir);
    status_set_note(session_title(s));
    prompt_rehome(dir);
    gitinfo_forget();
}

// The screen belongs to whichever tab is showing, so swapping tabs is a swap
// of that one thing. Anything buffered is flushed first, or it lands in the
// screen it was not written for.
void workspace_show(int index)
{
    if (index < 0 || index >= ntabs || index == cur)
        return;

    ui_flush();
    viewport_stash(tabs[cur].screen);
    viewport_adopt(tabs[index].screen);
    cur = index;

    block_forget();
    status_sticky_prompt(tabs[cur].sticky);
    status_sticky_busy(session_busy(tabs[cur].s));
    follow(tabs[cur].s);
    spin_follow();
    viewport_forget();
}

// What the sticky prompt says, kept per tab so it comes back with the tab.
static void sticky_set(int index, const char *line)
{
    free(tabs[index].sticky);
    tabs[index].sticky = line ? strdup(line) : NULL;
    if (index == cur)
        status_sticky_prompt(line);
}

/* --- who holds the drawing globals ---------------------------------------- */

// One tab at a time owns the viewport globals: the tab on screen, unless
// something has borrowed them. Borrows nest — a turn finishing inside the pump
// runs deferred commands, and those reach back into the workspace — so they
// are a stack rather than a single slot. Every enter() is matched by exactly
// one leave(), including the ones that swap nothing.
#define BORROW_MAX 16

struct borrow {
    int tab;                /* the tab holding the globals at this level */
    int hold;               /* whether the terminal is kept out of it */
    struct session *drawn;  /* what the session layer was drawing for */
};

static struct borrow borrows[BORROW_MAX];
static int           nborrow;

static struct borrow top(void)
{
    int d = nborrow < BORROW_MAX ? nborrow : BORROW_MAX;
    struct borrow b = {cur, 0, NULL};
    return d > 0 ? borrows[d - 1] : b;
}

// `hold` keeps what is drawn off the terminal even when the tab is the one in
// front: for a caller that owns the screen itself, such as an open modal.
static void enter_held(int index, int hold)
{
    struct borrow was = top();
    int to = index >= 0 && index < ntabs ? index : was.tab;

    if (nborrow < BORROW_MAX) {
        if (to != was.tab) {
            ui_flush();
            viewport_stash(tabs[was.tab].screen);
            viewport_adopt(tabs[to].screen);
        }
        borrows[nborrow].tab = to;
        borrows[nborrow].hold = hold || was.hold || to != cur;
        viewport_hold(borrows[nborrow].hold);
        // The session layer draws for whoever holds the screen, and for
        // nobody when this window holds no tabs.
        borrows[nborrow].drawn =
            session_set_drawing(to < ntabs ? tabs[to].s : NULL);
    }
    nborrow++;
}

static void enter(int index)
{
    enter_held(index, 0);
}

static void leave(void)
{
    if (nborrow <= 0)
        return;
    nborrow--;
    if (nborrow >= BORROW_MAX)
        return;

    int from = borrows[nborrow].tab;
    struct borrow back = top();
    session_set_drawing(borrows[nborrow].drawn);
    if (from != back.tab) {
        ui_flush();
        viewport_stash(tabs[from].screen);
        viewport_adopt(tabs[back.tab].screen);
    }
    viewport_hold(back.hold);
}

void workspace_render(int index, void (*fn)(struct session *s, void *ud), void *ud)
{
    if (!fn || index < 0 || index >= ntabs)
        return;
    enter(index);
    fn(tabs[index].s, ud);
    ui_flush();
    leave();
}

int workspace_find_id(const char *id)
{
    if (!id || !*id)
        return -1;
    for (int i = 0; i < ntabs; i++) {
        const char *mine = session_id(tabs[i].s);
        if (mine && !strcmp(mine, id))
            return i;
    }
    return -1;
}

int workspace_dump(int index, const char *path)
{
    if (index < 0 || index >= ntabs)
        return 0;
    enter(index);
    int ok = viewport_dump(path);
    leave();
    return ok;
}

static void drop(int index)
{
    // Nothing outside the workspace may keep this session: it is either freed
    // below or handed to the window that asked for it.
    tg_forget_session(tabs[index].s);
    cmd_forget_session(tabs[index].s);

    // A tab on screen keeps its rows in the globals and an empty stash; one in
    // the background is the other way round.
    if (index == cur)
        viewport_clear();
    viewport_state_free(tabs[index].screen);

    session_free(tabs[index].s);
    for (int i = 0; i < tabs[index].npending; i++) {
        free(tabs[index].pending[i].line);
        free(tabs[index].pending[i].shown);
    }
    tabs[index].npending = 0;
    free(tabs[index].sticky);
    tabs[index].sticky = NULL;

    for (int i = index; i + 1 < ntabs; i++)
        tabs[i] = tabs[i + 1];
    ntabs--;
    memset(&tabs[ntabs], 0, sizeof tabs[ntabs]);

    if (!ntabs)
        return;

    if (index < cur) {
        cur--;
    } else if (index == cur) {
        cur = index > 0 ? index - 1 : 0;
        // The globals were emptied above, so the tab taking over just adopts.
        viewport_adopt(tabs[cur].screen);
        block_forget();
        status_sticky_prompt(tabs[cur].sticky);
        status_sticky_busy(session_busy(tabs[cur].s));
        follow(tabs[cur].s);
        spin_follow();
        viewport_forget();
    }
}

int workspace_close(int index)
{
    if (index < 0 || index >= ntabs)
        return ntabs;
    drop(index);
    return ntabs;
}

int workspace_fds(int *out, int max)
{
    int n = 0;
    for (int i = 0; i < ntabs && n < max; i++) {
        // A turn in flight is reading the driver's stream itself; what wakes
        // the window then is the queue it fills.
        int fd = session_turn_running(tabs[i].s) ? session_wake_fd(tabs[i].s)
                                                 : session_idle_fd(tabs[i].s);
        if (fd >= 0)
            out[n++] = fd;
    }
    return n;
}

static void send_next(int index, int hold);

// What a turn leaves behind once it has ended: the window's business, not the
// session's. A modal owns the screen while it is up, and a deferred command
// can open one of its own, so the tail waits for the modal to go rather than
// nesting inside it.
static void settle_finished(int index, int hold)
{
    if (!tabs[index].finished || chrome_modal_active())
        return;
    tabs[index].finished = 0;

    struct session *s = tabs[index].s;
    enter_held(index, hold);
    if (on_finish)
        on_finish(s);
    leave();
    send_next(index, hold);
}

// `hold` is for a caller that owns the screen itself: the tabs still advance
// and what they draw still lands in their own transcript, but none of it
// reaches the terminal.
static int pump(int hold)
{
    int busy = 0;
    for (int i = 0; i < ntabs; i++) {
        struct session *s = tabs[i].s;
        int running = session_turn_running(s);

        enter_held(i, hold);
        if (running)
            busy |= session_turn_pump(s);
        else
            busy |= session_idle_pump(s) ? 1 : 0;
        leave();

        if (running && !session_turn_running(s))
            tabs[i].finished = 1;
        settle_finished(i, hold);
    }
    // A session that names itself while it is behind must not take the note
    // off the one in front.
    if (ntabs) {
        status_set_note(session_title(tabs[cur].s));
        status_sticky_busy(session_busy(tabs[cur].s));
    }
    spin_follow();
    return busy;
}

int workspace_pump(void)
{
    return pump(0);
}

int workspace_pump_quiet(void)
{
    int busy = pump(1);
    // A tab that paused the spinner while the screen was held left the block
    // erased and never redrawn: the caller's picture goes back.
    chrome_paint();
    return busy;
}

void workspace_settle(struct session *s)
{
    int at = workspace_index_of(s);
    if (at < 0 || !session_turn_running(s))
        return;

    enter(at);
    session_turn_wait(s);
    tabs[at].finished = 0;
    if (on_finish)
        on_finish(s);
    leave();
    send_next(at, 0);
    spin_follow();
}

int workspace_busy(void)
{
    for (int i = 0; i < ntabs; i++)
        if (session_busy(tabs[i].s))
            return 1;
    return 0;
}

// The next thing typed at a tab, once the turn it was typed behind is done.
static void send_next(int index, int hold)
{
    struct tab *t = &tabs[index];
    if (!t->npending || session_turn_running(t->s))
        return;

    struct pending p = t->pending[0];
    for (int i = 1; i < t->npending; i++)
        t->pending[i - 1] = t->pending[i];
    t->npending--;

    enter_held(index, hold);
    sticky_set(index, p.shown ? p.shown : p.line);
    // Held back until now, so this is where it joins the transcript.
    prompt_echo_message(p.shown ? p.shown : p.line);
    session_turn_begin(t->s, p.line);
    leave();
    free(p.line);
    free(p.shown);
    spin_follow();
}

int workspace_send(int index, const char *line, const char *shown)
{
    if (index < 0 || index >= ntabs || !line || !*line)
        return 0;
    struct tab *t = &tabs[index];

    if (session_turn_running(t->s)) {
        if (t->npending >= PENDING_MAX)
            return 0;
        struct pending p = {strdup(line), shown ? strdup(shown) : NULL};
        if (!p.line || (shown && !p.shown)) {
            free(p.line);
            free(p.shown);
            return 0;
        }
        t->pending[t->npending++] = p;
        return 1;
    }

    sticky_set(index, shown ? shown : line);
    enter(index);
    int ok = session_turn_begin(t->s, line);
    leave();
    spin_follow();
    return ok;
}

int workspace_queued(int index)
{
    return index >= 0 && index < ntabs ? tabs[index].npending : 0;
}

const char *workspace_status(const struct session *s)
{
    if (!s)
        return "finished";
    if (session_busy(s))
        return "working";
    if (session_failed_prompt(s))
        return "errored";
    return "finished";
}
