#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agenttabs.h"
#include "vendor/cJSON.h"

static int check_record(const char *path, const char *status, int with_usage)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *row = cJSON_Parse(buf);
    const char *agent = row ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(row, "agent")) : NULL;
    const char *actual = row ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(row, "status")) : NULL;
    const char *pane = row ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(row, "tmux_pane")) : NULL;
    cJSON *ts = row ? cJSON_GetObjectItemCaseSensitive(row, "ts") : NULL;
    cJSON *pct = row ? cJSON_GetObjectItemCaseSensitive(row, "usage_percent") : NULL;
    cJSON *reset = row ? cJSON_GetObjectItemCaseSensitive(row, "usage_resets_at") : NULL;
    cJSON *window = row ? cJSON_GetObjectItemCaseSensitive(row, "usage_window_minutes") : NULL;
    cJSON *uts = row ? cJSON_GetObjectItemCaseSensitive(row, "usage_ts") : NULL;
    int ok = agent && !strcmp(agent, "codex") && actual && !strcmp(actual, status) &&
             pane && !strcmp(pane, "%42") && cJSON_IsNumber(ts) && ts->valuedouble > 0;
    if (with_usage)
        ok = ok && cJSON_IsNumber(pct) && pct->valueint == 37 &&
             cJSON_IsNumber(reset) && reset->valuedouble == 2000000000.0 &&
             cJSON_IsNumber(window) && window->valueint == 10080 &&
             cJSON_IsNumber(uts) && uts->valuedouble > 0;
    else
        ok = ok && !pct && !reset && !window && !uts;
    cJSON_Delete(row);
    return ok;
}

int main(void)
{
    char root[] = "/tmp/simple-agent-tabs-XXXXXX";
    if (!mkdtemp(root)) {
        perror("agenttabstest: mkdtemp");
        return 1;
    }
    setenv("AGENT_TABS_STATE_DIR", root, 1);
    setenv("TMUX_PANE", "%42", 1);
    agenttabs_begin("codex");

    char agents[512], record[640];
    snprintf(agents, sizeof agents, "%s/agents", root);
    snprintf(record, sizeof record, "%s/%ld.json", agents, (long)getpid());
    if (!check_record(record, "finished", 0)) {
        fputs("agenttabstest: initial provider record is invalid\n", stderr);
        return 1;
    }

    agenttabs_working();
    agenttabs_usage(37, 2000000000L, 10080);
    if (!check_record(record, "working", 1)) {
        fputs("agenttabstest: working quota record is invalid\n", stderr);
        return 1;
    }

    agenttabs_finished();
    if (!check_record(record, "finished", 1)) {
        fputs("agenttabstest: status rewrite discarded quota\n", stderr);
        return 1;
    }

    unlink(record);
    rmdir(agents);
    rmdir(root);
    puts("agenttabstest: ok");
    return 0;
}
