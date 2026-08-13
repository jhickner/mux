#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/agents/codex/codex.h"
#include "vendor/cJSON.h"

static int abort_turn;

static int should_abort(void)
{
    return abort_turn;
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
            respond(id, turns == 1
                ? "{\"turn\":{\"id\":\"turn-1\"}}"
                : "{\"turn\":{\"id\":\"turn-2\"}}");
            printf("{\"method\":\"item/agentMessage/delta\","
                   "\"params\":{\"delta\":\"%s\"}}\n",
                   turns == 1 ? "partial" : "done");
            if (turns > 1) {
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

    codex_opts opts = { .cli_path = argv[0] };
    codex_client *client = codex_start(&opts);
    if (!client) {
        fprintf(stderr, "codextest: could not start mock app-server\n");
        return 1;
    }

    codex_set_abort_check(client, should_abort);
    abort_turn = 1;
    codex_result meta = {0};
    char *reply = codex_send_ex(client, "interrupt me", &meta);
    if (!reply || strcmp(reply, "partial") || !meta.interrupted) {
        fprintf(stderr, "codextest: interrupted turn was reported as a failure\n");
        free(reply);
        codex_stop(client);
        return 1;
    }
    free(reply);

    abort_turn = 0;
    reply = codex_send(client, "continue");
    if (!reply || strcmp(reply, "done")) {
        fprintf(stderr, "codextest: process was not reusable after interrupt\n");
        free(reply);
        codex_stop(client);
        return 1;
    }
    free(reply);
    codex_stop(client);
    puts("codextest: ok");
    return 0;
}
