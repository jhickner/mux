#include "chattabs.h"

#include <stdlib.h>
#include <string.h>

#include "cmd.h"
#include "session.h"
#include "settings.h"
#include "tg.h"

static struct session *tabs[CHATTABS_MAX];
static int             ntabs;
static int             cur;
static void          (*prepare)(struct session *s);
static void          (*background)(struct session *s, int index);

void chattabs_on_open(void (*fn)(struct session *s)) { prepare = fn; }
void chattabs_on_background(void (*fn)(struct session *s, int index))
{
    background = fn;
}

int chattabs_count(void) { return ntabs; }
int chattabs_index(void) { return cur; }

struct session *chattabs_current(void)
{
    return ntabs ? tabs[cur] : NULL;
}

struct session *chattabs_at(int index)
{
    return index >= 0 && index < ntabs ? tabs[index] : NULL;
}

int chattabs_index_of(const struct session *s)
{
    for (int i = 0; i < ntabs; i++)
        if (tabs[i] == s)
            return i;
    return -1;
}

int chattabs_begin(struct session *first)
{
    ntabs = 0;
    cur = 0;
    if (!first)
        return 0;
    tabs[ntabs++] = first;
    return 1;
}

void chattabs_end(void)
{
    for (int i = 0; i < ntabs; i++) {
        cmd_forget_session(tabs[i]);
        tg_forget_session(tabs[i]);
        session_free(tabs[i]);
    }
    ntabs = 0;
    cur = 0;
}

int chattabs_open(const char *cwd, const char *id)
{
    if (ntabs >= CHATTABS_MAX)
        return -1;

    const struct session *from = chattabs_current();
    const char *backend = from ? session_backend(from) : NULL;
    if (!backend)
        return -1;
    if (!cwd || !*cwd)
        cwd = session_cwd(from);

    struct session *s = session_new(backend, cwd, session_model(from),
                                    session_effort(from));
    if (!s)
        return -1;

    session_set_thinking(s, settings_get_int(SETTING_THINKING, 1));
    session_set_compact(s, settings_get_int(SETTING_COMPACT, 0));
    session_set_permission(s, session_permission_name(
        settings_get_int(SETTING_PERMISSION, session_permission_default())));
    session_adopt_id(s, id);
    if (prepare)
        prepare(s);

    if (!session_start(s)) {
        session_free(s);
        return -1;
    }

    tabs[ntabs++] = s;
    cur = ntabs - 1;

    // With more than one conversation open, what each one is called is the
    // only thing that tells them apart in a list.
    for (int i = 0; i < ntabs; i++)
        session_set_naming(tabs[i], 1);
    return cur;
}

int chattabs_show(int index)
{
    if (index < 0 || index >= ntabs)
        return 0;
    cur = index;
    session_set_unseen(tabs[cur], 0);
    return 1;
}

int chattabs_find_id(const char *id)
{
    if (!id || !*id)
        return -1;
    for (int i = 0; i < ntabs; i++) {
        const char *mine = session_id(tabs[i]);
        if (mine && !strcmp(mine, id))
            return i;
    }
    return -1;
}

int chattabs_close(int index)
{
    if (index < 0 || index >= ntabs)
        return ntabs;

    // Nothing outside this module may keep the session past here.
    cmd_forget_session(tabs[index]);
    tg_forget_session(tabs[index]);
    session_free(tabs[index]);

    for (int i = index; i + 1 < ntabs; i++)
        tabs[i] = tabs[i + 1];
    tabs[--ntabs] = NULL;

    if (!ntabs)
        cur = 0;
    else if (index < cur)
        cur--;
    else if (index == cur)
        cur = index > 0 ? index - 1 : 0;
    return ntabs;
}

int chattabs_unseen(int index)
{
    return index >= 0 && index < ntabs ? session_unseen(tabs[index]) : 0;
}

int chattabs_fds(int *out, int max)
{
    int n = 0;
    for (int i = 0; i < ntabs && n < max; i++) {
        // A turn in flight reads the driver's stream on its own thread; what
        // wakes this one then is the queue it fills.
        int fd = session_turn_running(tabs[i]) ? session_wake_fd(tabs[i])
                                               : session_idle_fd(tabs[i]);
        if (fd >= 0)
            out[n++] = fd;
    }
    return n;
}

void chattabs_pump(void)
{
    for (int i = 0; i < ntabs; i++) {
        struct session *s = tabs[i];
        int running = session_turn_running(s);
        if (running)
            session_turn_pump(s);
        else
            session_idle_pump(s);

        if (!running || session_turn_running(s))
            continue;

        // The turn the chat is waiting on is reported by whoever asked for it.
        // One that ended behind its back is this module's to announce.
        if (i == cur)
            continue;
        session_set_unseen(s, 1);
        if (background)
            background(s, i);
    }
}

int chattabs_busy(void)
{
    for (int i = 0; i < ntabs; i++)
        if (session_busy(tabs[i]))
            return 1;
    return 0;
}
