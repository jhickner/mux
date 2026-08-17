#include "agenttabs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char record[4200];
static char hook_dir[4200];
static char agent[32];
static const char *current_status;
static int usage_percent = -1;
static long usage_resets_at;
static long usage_window_minutes;
static time_t usage_updated_at;

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

static int pane_id(const char *s)
{
    if (!s || *s != '%' || !s[1])
        return 0;
    for (const char *p = s + 1; *p; p++)
        if (*p < '0' || *p > '9')
            return 0;
    return 1;
}

static void write_record(void)
{
    if (!record[0] || !current_status)
        return;

    char tmp[4300];
    snprintf(tmp, sizeof tmp, "%s.tmp", record);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;

    const char *pane = getenv("TMUX_PANE");
    fprintf(f, "{\"agent\":\"%s\",\"pid\":%ld,\"status\":\"%s\",\"ts\":%ld",
            agent, (long)getpid(), current_status, (long)time(NULL));
    if (pane_id(pane))
        fprintf(f, ",\"tmux_pane\":\"%s\"", pane);
    if (usage_percent >= 0) {
        fprintf(f, ",\"usage_percent\":%d,\"usage_resets_at\":%ld"
                   ",\"usage_window_minutes\":%ld,\"usage_ts\":%ld",
                usage_percent, usage_resets_at, usage_window_minutes,
                (long)usage_updated_at);
    }
    fprintf(f, "}\n");

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

void agenttabs_begin(const char *backend)
{

    if (!getenv("TMUX_PANE"))
        return;

    if (!backend || !*backend || strlen(backend) >= sizeof agent)
        return;
    for (const char *p = backend; *p; p++)
        if ((*p < 'a' || *p > 'z') && (*p < '0' || *p > '9') && *p != '-' && *p != '_')
            return;
    snprintf(agent, sizeof agent, "%s", backend);

    char dir[4096];
    struct stat st;
    if (!state_dir(dir, sizeof dir) || stat(dir, &st) != 0)
        return;

    char agents[4200];
    if (snprintf(agents, sizeof agents, "%s/agents", dir) >= (int)sizeof agents)
        return;
    if (mkdir(agents, 0700) != 0 && errno != EEXIST)
        return;

    if (snprintf(hook_dir, sizeof hook_dir, "%s/state", dir) >= (int)sizeof hook_dir)
        return;
    if (snprintf(record, sizeof record, "%s/%ld.json", agents, (long)getpid()) >=
        (int)sizeof record)
        return;

    setenv("AGENT_TABS_WRAPPED", "1", 1);

    atexit(drop_record);

    agenttabs_finished();
}

static void write_status(const char *status)
{
    current_status = status;
    write_record();
}

void agenttabs_working(void)  { write_status("working"); }
void agenttabs_finished(void) { write_status("finished"); }
void agenttabs_errored(void)  { write_status("errored"); }

void agenttabs_usage(int percent, long resets_at, long window_minutes)
{
    if (!record[0] || percent < 0 || percent > 100)
        return;
    resets_at = resets_at > 0 ? resets_at : 0;
    window_minutes = window_minutes > 0 ? window_minutes : 0;
    if (percent == usage_percent && resets_at == usage_resets_at &&
        window_minutes == usage_window_minutes)
        return;
    usage_percent = percent;
    usage_resets_at = resets_at;
    usage_window_minutes = window_minutes;
    usage_updated_at = time(NULL);
    write_record();
}

void agenttabs_forget_hook(const char *id)
{

    if (!record[0] || !id || !*id || strchr(id, '/'))
        return;

    char path[4300];
    snprintf(path, sizeof path, "%s/%s.json", hook_dir, id);
    unlink(path);
}
