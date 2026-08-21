#include "livelist.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "session.h"
#include "text.h"
#include "title.h"
#include "vendor/cJSON.h"

#define MAX_LIVE 200
#define MAX_SLOTS 32

static int  publishing;
static char dir[4200];
static char tmux_window[32];
static char tmux_wname[64];
static char tmux_pane_index[8];

// A record is named for the process and the slot within it, so one window
// holding several conversations publishes one file each.
static const struct session *slots[MAX_SLOTS];

static int slot_of(const struct session *s)
{
    int free_at = -1;
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (slots[i] == s)
            return i;
        if (!slots[i] && free_at < 0)
            free_at = i;
    }
    if (free_at < 0)
        return -1;
    slots[free_at] = s;
    return free_at;
}

static int record_path(int slot, char *out, size_t size)
{
    if (!dir[0])
        return 0;
    return (size_t)snprintf(out, size, "%s/%ld-%d.json", dir, (long)getpid(), slot) < size;
}

static void drop_all(void)
{
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (!slots[i])
            continue;
        char path[4400];
        if (record_path(i, path, sizeof path))
            unlink(path);
        slots[i] = NULL;
    }
}

// Where the records live. The override is for testing: a mux driven by a
// script must not be able to see, or take, the windows someone is using.
static int live_dir(char *out, size_t size)
{
    const char *env = getenv("MUX_LIVE_DIR");
    if (env && *env)
        return (size_t)snprintf(out, size, "%s", env) < size;

    char base[4096];
    if (!path_config_dir(base, sizeof base))
        return 0;
    return (size_t)snprintf(out, size, "%s/live", base) < size;
}

// tmux is asked where we are at most once every few seconds: a window can be
// renamed, and a pane moved to another window, while mux is running.
static void tmux_where(void)
{
    static long asked;
    static int  have;

    long now = (long)time(NULL);
    if (have && now - asked < 5)
        return;
    asked = now;

    const char *pane = getenv("TMUX_PANE");
    if (!getenv("TMUX") || !pane || !*pane)
        return;

    char cmd[256];
    snprintf(cmd, sizeof cmd,
             "tmux display-message -p -t '%s' "
             "'#{window_id}\t#{window_index}:#{window_name}\t#{pane_index}' 2>/dev/null",
             pane);
    FILE *p = popen(cmd, "r");
    if (!p)
        return;
    char line[256];
    char *got = fgets(line, sizeof line, p);
    pclose(p);
    if (!got)
        return;

    line[strcspn(line, "\n")] = '\0';
    char *name = strchr(line, '\t');
    if (!name)
        return;
    *name++ = '\0';
    char *index = strchr(name, '\t');
    if (index)
        *index++ = '\0';

    snprintf(tmux_window, sizeof tmux_window, "%s", line);
    snprintf(tmux_wname, sizeof tmux_wname, "%s", name);
    snprintf(tmux_pane_index, sizeof tmux_pane_index, "%s", index ? index : "");
    have = 1;
}

const char *livelist_tmux_window(void)
{
    tmux_where();
    return tmux_window;
}

const char *livelist_tmux_window_name(void)
{
    tmux_where();
    return tmux_wname;
}

void livelist_begin(void)
{
    if (!live_dir(dir, sizeof dir)) {
        dir[0] = '\0';
        return;
    }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        dir[0] = '\0';
        return;
    }
    publishing = 1;
    atexit(drop_all);
}

void livelist_publish(const struct session *s, const char *status)
{
    if (!publishing || !s || !status)
        return;
    int slot = slot_of(s);
    if (slot < 0)
        return;

    char path[4400], tmp[4500];
    if (!record_path(slot, path, sizeof path))
        return;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f)
        return;

    const char *id = session_id(s);
    char name[200] = "";
    if (id)
        title_lookup(id, name, sizeof name);

    cJSON *rec = cJSON_CreateObject();
    if (!rec) {
        fclose(f);
        unlink(tmp);
        return;
    }
    cJSON_AddNumberToObject(rec, "pid", (double)getpid());
    cJSON_AddNumberToObject(rec, "slot", slot);
    cJSON_AddStringToObject(rec, "backend", session_backend(s));
    cJSON_AddStringToObject(rec, "model", session_model(s));
    cJSON_AddStringToObject(rec, "label", session_model_label(s));
    cJSON_AddStringToObject(rec, "effort", session_effort(s));
    cJSON_AddStringToObject(rec, "cwd", session_cwd(s) ? session_cwd(s) : "");
    cJSON_AddStringToObject(rec, "id", id ? id : "");
    cJSON_AddStringToObject(rec, "title", name);
    cJSON_AddStringToObject(rec, "status", status);
    cJSON_AddNumberToObject(rec, "ts", (double)time(NULL));
    const char *pane = getenv("TMUX_PANE");
    if (pane && *pane)
        cJSON_AddStringToObject(rec, "pane", pane);
    tmux_where();
    if (tmux_window[0]) {
        cJSON_AddStringToObject(rec, "window", tmux_window);
        cJSON_AddStringToObject(rec, "wname", tmux_wname);
        cJSON_AddStringToObject(rec, "pane_index", tmux_pane_index);
    }

    char *text = cJSON_PrintUnformatted(rec);
    cJSON_Delete(rec);
    if (!text) {
        fclose(f);
        unlink(tmp);
        return;
    }
    int ok = fprintf(f, "%s\n", text) > 0;
    free(text);
    if (fclose(f) == 0 && ok)
        rename(tmp, path);
    else
        unlink(tmp);
}

void livelist_forget(const struct session *s)
{
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (slots[i] != s)
            continue;
        char path[4400];
        if (record_path(i, path, sizeof path))
            unlink(path);
        slots[i] = NULL;
        return;
    }
}

int livelist_alive(long pid)
{
    if (pid <= 0)
        return 0;
    if (kill((pid_t)pid, 0) == 0)
        return 1;
    return errno == EPERM;
}

static void copy_str(char *out, size_t size, const cJSON *rec, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(rec, key));
    snprintf(out, size, "%s", s ? s : "");
}

static long number(const cJSON *rec, const char *key)
{
    const cJSON *n = cJSON_GetObjectItem(rec, key);
    return cJSON_IsNumber(n) ? (long)cJSON_GetNumberValue(n) : 0;
}

static int newer(const void *a, const void *b)
{
    const struct live_session *x = a, *y = b;
    if (x->ts != y->ts)
        return x->ts < y->ts ? 1 : -1;
    if (x->pid != y->pid)
        return x->pid < y->pid ? -1 : 1;
    return x->slot - y->slot;
}

int livelist_load(struct live_session **out)
{
    *out = NULL;

    char where[4200];
    if (!live_dir(where, sizeof where))
        return 0;

    DIR *d = opendir(where);
    if (!d)
        return 0;

    struct live_session *list = calloc(MAX_LIVE, sizeof *list);
    if (!list) {
        closedir(d);
        return 0;
    }

    int count = 0;
    struct dirent *e;
    while (count < MAX_LIVE && (e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".json") != 0)
            continue;

        char path[9000];
        snprintf(path, sizeof path, "%s/%s", where, e->d_name);

        size_t len = 0;
        char *text = text_slurp(path, 64 * 1024, &len);
        if (!text)
            continue;
        cJSON *rec = cJSON_ParseWithLength(text, len);
        free(text);
        if (!rec)
            continue;

        struct live_session *v = &list[count];
        v->pid = number(rec, "pid");
        v->slot = (int)number(rec, "slot");
        v->ts = number(rec, "ts");
        copy_str(v->backend, sizeof v->backend, rec, "backend");
        copy_str(v->model, sizeof v->model, rec, "model");
        copy_str(v->label, sizeof v->label, rec, "label");
        copy_str(v->effort, sizeof v->effort, rec, "effort");
        copy_str(v->cwd, sizeof v->cwd, rec, "cwd");
        copy_str(v->id, sizeof v->id, rec, "id");
        copy_str(v->title, sizeof v->title, rec, "title");
        copy_str(v->status, sizeof v->status, rec, "status");
        copy_str(v->window, sizeof v->window, rec, "window");
        copy_str(v->wname, sizeof v->wname, rec, "wname");
        copy_str(v->pane_index, sizeof v->pane_index, rec, "pane_index");
        copy_str(v->pane, sizeof v->pane, rec, "pane");
        cJSON_Delete(rec);

        // A window that died without unlinking its records: nothing else will
        // clean them up, so the first reader past them does.
        if (!livelist_alive(v->pid)) {
            unlink(path);
            continue;
        }
        v->mine = v->pid == (long)getpid();
        count++;
    }
    closedir(d);

    if (!count) {
        free(list);
        return 0;
    }
    qsort(list, (size_t)count, sizeof *list, newer);
    *out = list;
    return count;
}
