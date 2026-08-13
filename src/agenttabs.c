#include "agenttabs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Both empty until agenttabs_begin() claims the tab; every entry point below
 * checks `record` and does nothing without one. */
static char record[4200];    /* <state>/agents/<pid>.json, the row we own    */
static char hook_dir[4200];  /* <state>/state, where the CLI's hook writes   */

/* Mirrors the plugin's scripts/paths.sh agent_tabs_state_dir(): the tmux config
 * tree, falling back to the legacy ~/.tmux only when it is the one that
 * exists. */
static int state_dir(char *out, size_t size)
{
    const char *env = getenv("AGENT_TABS_STATE_DIR");
    if (env && *env) {
        snprintf(out, size, "%s", env);
        return 1;
    }

    const char *home = getenv("HOME");
    if (!home || !*home)
        return 0;

    char base[4096], legacy[4096];
    struct stat st;
    snprintf(base, sizeof base, "%s/.config/tmux", home);
    snprintf(legacy, sizeof legacy, "%s/.tmux", home);
    if ((stat(base, &st) != 0 || !S_ISDIR(st.st_mode)) &&
        stat(legacy, &st) == 0 && S_ISDIR(st.st_mode))
        snprintf(base, sizeof base, "%s", legacy);

    snprintf(out, size, "%s/tmux-agent-tabs", base);
    return 1;
}

/* A pane id is "%<digits>", so one that looks right needs no JSON escaping.
 * Anything else is dropped rather than quoted: the field is optional, and the
 * plugin falls back to walking our pid up to its pane. */
static int pane_id(const char *s)
{
    if (!s || *s != '%' || !s[1])
        return 0;
    for (const char *p = s + 1; *p; p++)
        if (*p < '0' || *p > '9')
            return 0;
    return 1;
}

static void write_status(const char *status)
{
    if (!record[0])
        return;

    char tmp[4300];
    snprintf(tmp, sizeof tmp, "%s.tmp", record);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;

    const char *pane = getenv("TMUX_PANE");
    fprintf(f, "{\"pid\":%ld,\"status\":\"%s\"", (long)getpid(), status);
    if (pane_id(pane))
        fprintf(f, ",\"tmux_pane\":\"%s\"", pane);
    fprintf(f, "}\n");

    /* The plugin re-reads this file every second, so it must never catch one
     * half-written. */
    if (fclose(f) == 0)
        rename(tmp, record);
    else
        unlink(tmp);
}

static void drop_record(void)
{
    if (record[0])
        unlink(record);
}

void agenttabs_begin(void)
{
    /* No pane, no tab to report on. */
    if (!getenv("TMUX_PANE"))
        return;

    char dir[4096];
    struct stat st;
    if (!state_dir(dir, sizeof dir) || stat(dir, &st) != 0)
        return;   /* the plugin has never run here — nothing reads what we write */

    char agents[4200];
    snprintf(agents, sizeof agents, "%s/agents", dir);
    if (mkdir(agents, 0700) != 0 && errno != EEXIST)
        return;

    snprintf(hook_dir, sizeof hook_dir, "%s/state", dir);
    snprintf(record, sizeof record, "%s/%ld.json", agents, (long)getpid());

    /* Silence the CLI's own hook, which would otherwise write a second row for
     * this same window: its status outranks ours whenever it is the staler of
     * the two, which is exactly what happens on an interrupt. The child reads
     * this from the environment it inherits, so it has to be set before the
     * CLI is spawned. */
    setenv("AGENT_TABS_WRAPPED", "1", 1);

    /* The row outlives every exit path, so retire it from one place. A leaked
     * record is not fatal — the plugin drops rows whose pid is gone — but it
     * would sit in agents/ until something else cleans up. */
    atexit(drop_record);

    agenttabs_finished();
}

void agenttabs_working(void)  { write_status("working"); }
void agenttabs_finished(void) { write_status("finished"); }
void agenttabs_errored(void)  { write_status("errored"); }

void agenttabs_forget_hook(const char *id)
{
    /* Session ids come from the CLI and are uuids; refuse anything that could
     * name a path outside the hook's own directory. */
    if (!record[0] || !id || !*id || strchr(id, '/'))
        return;

    char path[4300];
    snprintf(path, sizeof path, "%s/%s.json", hook_dir, id);
    unlink(path);
}
