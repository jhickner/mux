#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/agents/codex/codex.h"
#include "vendor/cJSON.h"

static int abort_turn;
static codex_client *test_client;
static long live_tokens, live_window;
static int shell_starts, edit_starts, tool_results;
static char shell_input[512], shell_output[512], edit_input[1024], edit_diff[1024];

static int should_abort(void)
{
    codex_usage(test_client, &live_tokens, &live_window);
    return abort_turn && live_tokens > 0;
}

static void capture_event(void *ud, const codex_event *ev)
{
    (void)ud;
    if (ev->kind == CODEX_EV_TOOL && ev->name && !strcmp(ev->name, "Shell")) {
        shell_starts++;
        snprintf(shell_input, sizeof shell_input, "%s",
                 ev->input_json ? ev->input_json : "");
    } else if (ev->kind == CODEX_EV_TOOL && ev->name && !strcmp(ev->name, "Edit")) {
        edit_starts++;
        snprintf(edit_input, sizeof edit_input, "%s",
                 ev->input_json ? ev->input_json : "");
    } else if (ev->kind == CODEX_EV_TOOL_RESULT) {
        tool_results++;
        if (ev->diff) {
            snprintf(edit_diff, sizeof edit_diff, "%s", ev->diff);
        } else {
            snprintf(shell_output, sizeof shell_output, "%s", ev->text ? ev->text : "");
        }
    }
}

static void respond(int id, const char *result)
{
    printf("{\"id\":%d,\"result\":%s}\n", id, result);
    fflush(stdout);
}

static int mock_server(void)
{
    char *line = NULL;
    size_t cap = 0;
    int turns = 0;

    while (getline(&line, &cap, stdin) >= 0) {
        cJSON *msg = cJSON_Parse(line);
        cJSON *idj = msg ? cJSON_GetObjectItemCaseSensitive(msg, "id") : NULL;
        const char *method = msg ? cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(msg, "method")) : NULL;
        int id = cJSON_IsNumber(idj) ? idj->valueint : 0;

        if (method && !strcmp(method, "initialize")) {
            respond(id, "{}");
        } else if (method && !strcmp(method, "thread/start")) {
            respond(id, "{\"thread\":{\"id\":\"thread-1\"}}");
        } else if (method && !strcmp(method, "turn/start")) {
            turns++;
            cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
            cJSON *effort = params ? cJSON_GetObjectItemCaseSensitive(params, "effort") : NULL;
            int valid_effort =
                (turns == 1 && cJSON_IsString(effort) && !strcmp(effort->valuestring, "high")) ||
                (turns == 2 && cJSON_IsString(effort) && !strcmp(effort->valuestring, "low")) ||
                (turns == 3 && cJSON_IsNull(effort));
            if (!valid_effort) {
                respond(id, "{}");
                cJSON_Delete(msg);
                continue;
            }
            char started[64];
            snprintf(started, sizeof started, "{\"turn\":{\"id\":\"turn-%d\"}}", turns);
            respond(id, started);
            printf("{\"method\":\"thread/tokenUsage/updated\",\"params\":{"
                   "\"threadId\":\"thread-1\",\"turnId\":\"turn-%d\","
                   "\"tokenUsage\":{\"last\":{\"totalTokens\":%d},"
                   "\"total\":{\"totalTokens\":9999},"
                   "\"modelContextWindow\":1000}}}\n",
                   turns, turns == 1 ? 120 : 240);
            printf("{\"method\":\"item/agentMessage/delta\","
                   "\"params\":{\"delta\":\"%s\"}}\n",
                   turns == 1 ? "partial" : turns == 2 ? "done" : "reset");
            if (turns > 1) {
                if (turns == 2) {
                    printf("{\"method\":\"item/started\",\"params\":{\"item\":{"
                           "\"type\":\"commandExecution\",\"id\":\"cmd-1\","
                           "\"command\":\"sed -n '1,2p' src/session.c\","
                           "\"cwd\":\"/project\",\"status\":\"inProgress\","
                           "\"commandActions\":[]}}}\n");
                    printf("{\"method\":\"item/completed\",\"params\":{\"item\":{"
                           "\"type\":\"commandExecution\",\"id\":\"cmd-1\","
                           "\"command\":\"sed -n '1,2p' src/session.c\","
                           "\"cwd\":\"/project\",\"status\":\"completed\","
                           "\"commandActions\":[],"
                           "\"aggregatedOutput\":\"line one\\nline two\\n\"}}}\n");
                    printf("{\"method\":\"item/started\",\"params\":{\"item\":{"
                           "\"type\":\"fileChange\",\"id\":\"edit-1\","
                           "\"status\":\"inProgress\",\"changes\":[{"
                           "\"path\":\"src/session.c\",\"kind\":{\"type\":\"update\"},"
                           "\"diff\":\"@@ -1 +1 @@\\n-old\\n+new\\n\"},{"
                           "\"path\":\"src/session.h\",\"kind\":{\"type\":\"update\"},"
                           "\"diff\":\"@@ -2 +2 @@\\n-before\\n+after\\n\"}]}}}\n");
                    printf("{\"method\":\"item/completed\",\"params\":{\"item\":{"
                           "\"type\":\"fileChange\",\"id\":\"edit-1\","
                           "\"status\":\"completed\",\"changes\":[{"
                           "\"path\":\"src/session.c\",\"kind\":{\"type\":\"update\"},"
                           "\"diff\":\"@@ -1 +1 @@\\n-old\\n+new\\n\"},{"
                           "\"path\":\"src/session.h\",\"kind\":{\"type\":\"update\"},"
                           "\"diff\":\"@@ -2 +2 @@\\n-before\\n+after\\n\"}]}}}\n");
                }
                printf("{\"method\":\"turn/completed\",\"params\":{"
                       "\"turn\":{\"status\":\"completed\"}}}\n");
            }
            fflush(stdout);
        } else if (method && !strcmp(method, "turn/interrupt")) {
            respond(id, "{}");
            printf("{\"method\":\"turn/completed\",\"params\":{"
                   "\"turn\":{\"status\":\"interrupted\"}}}\n");
            fflush(stdout);
        }
        cJSON_Delete(msg);
    }
    free(line);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "app-server"))
        return mock_server();

    codex_opts opts = { .cli_path = argv[0], .effort = "high" };
    codex_client *client = codex_start(&opts);
    if (!client) {
        fprintf(stderr, "codextest: could not start mock app-server\n");
        return 1;
    }

    test_client = client;
    codex_set_abort_check(client, should_abort);
    codex_set_event_cb(client, capture_event, NULL);
    abort_turn = 1;
    codex_result meta = {0};
    char *reply = codex_send_ex(client, "interrupt me", &meta);
    if (!reply || strcmp(reply, "partial") || !meta.interrupted ||
        meta.context_tokens != 120 || meta.context_window != 1000 ||
        live_tokens != 120 || live_window != 1000) {
        fprintf(stderr, "codextest: interrupted turn was reported as a failure\n");
        free(reply);
        codex_stop(client);
        return 1;
    }
    free(reply);

    abort_turn = 0;
    if (!codex_set_effort(client, "low")) {
        fprintf(stderr, "codextest: could not change effort\n");
        codex_stop(client);
        return 1;
    }
    reply = codex_send(client, "continue");
    if (!reply || strcmp(reply, "done")) {
        fprintf(stderr, "codextest: process was not reusable after interrupt\n");
        free(reply);
        codex_stop(client);
        return 1;
    }
    free(reply);

    cJSON *shell = cJSON_Parse(shell_input);
    const char *command = shell ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(shell, "command")) : NULL;
    const char *cwd = shell ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(shell, "cwd")) : NULL;
    cJSON *edit = cJSON_Parse(edit_input);
    const char *file_path = edit ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(edit, "file_path")) : NULL;
    cJSON *changes = edit ? cJSON_GetObjectItemCaseSensitive(edit, "changes") : NULL;
    if (shell_starts != 1 || edit_starts != 1 || tool_results != 2 ||
        !command || strcmp(command, "sed -n '1,2p' src/session.c") ||
        !cwd || strcmp(cwd, "/project") ||
        strcmp(shell_output, "line one\nline two\n") ||
        !file_path || strcmp(file_path, "src/session.c") ||
        !cJSON_IsArray(changes) || cJSON_GetArraySize(changes) != 2 ||
        strcmp(edit_diff,
               "@@file src/session.c\n@@ -1 +1 @@\n-old\n+new\n"
               "@@file src/session.h\n@@ -2 +2 @@\n-before\n+after\n")) {
        fprintf(stderr, "codextest: structured tool events were not preserved\n");
        cJSON_Delete(shell);
        cJSON_Delete(edit);
        codex_stop(client);
        return 1;
    }
    cJSON_Delete(shell);
    cJSON_Delete(edit);

    codex_usage(client, &live_tokens, &live_window);
    if (live_tokens != 240 || live_window != 1000) {
        fprintf(stderr, "codextest: latest context usage was not retained\n");
        codex_stop(client);
        return 1;
    }

    if (!codex_set_effort(client, NULL)) {
        fprintf(stderr, "codextest: could not reset effort\n");
        codex_stop(client);
        return 1;
    }
    reply = codex_send(client, "use the default");
    if (!reply || strcmp(reply, "reset")) {
        fprintf(stderr, "codextest: default effort was not sent as null\n");
        free(reply);
        codex_stop(client);
        return 1;
    }
    free(reply);

    codex_reset(client);
    codex_usage(client, &live_tokens, &live_window);
    if (live_tokens || live_window) {
        fprintf(stderr, "codextest: reset retained stale context usage\n");
        codex_stop(client);
        return 1;
    }
    codex_stop(client);
    puts("codextest: ok");
    return 0;
}
