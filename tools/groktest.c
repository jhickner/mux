#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/agents/grok/grok.h"
#include "vendor/cJSON.h"

static void respond(int id, const char *result)
{
    printf("{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}\n", id, result);
    fflush(stdout);
}

static int mock_server(int argc, char **argv)
{
    int model = 0;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "-m") && !strcmp(argv[i + 1], "grok-test"))
            model = 1;
    if (!model)
        return 2;

    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, stdin) >= 0) {
        cJSON *msg = cJSON_Parse(line);
        cJSON *idj = msg ? cJSON_GetObjectItemCaseSensitive(msg, "id") : NULL;
        const char *method = msg ? cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(msg, "method")) : NULL;
        int id = cJSON_IsNumber(idj) ? idj->valueint : 0;

        if (method && !strcmp(method, "initialize")) {
            respond(id, "{}");
        } else if (method && !strcmp(method, "session/new")) {
            cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
            cJSON *meta = params ? cJSON_GetObjectItemCaseSensitive(params, "_meta") : NULL;
            if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(meta, "x.ai/persist"))) {
                printf("{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{"
                       "\"code\":-32602,\"message\":\"session was persistent\"}}\n", id);
                fflush(stdout);
            } else {
                respond(id, "{\"sessionId\":\"grok-session-1\"}");
            }
        } else if (method && !strcmp(method, "session/prompt")) {
            printf("{\"jsonrpc\":\"2.0\",\"method\":\"session/update\","
                   "\"params\":{\"update\":{\"sessionUpdate\":"
                   "\"agent_message_chunk\",\"content\":{\"type\":\"text\","
                   "\"text\":\"A useful title\"}}}}\n");
            respond(id, "{\"stopReason\":\"end_turn\"}");
        }
        cJSON_Delete(msg);
    }
    free(line);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "agent"))
        return mock_server(argc, argv);

    grok_opts opts = { .cli_path = argv[0], .model = "grok-test", .no_session = 1 };
    grok_client *client = grok_start(&opts);
    if (!client) {
        fputs("groktest: could not start mock ACP process\n", stderr);
        return 1;
    }
    char *reply = grok_send(client, "name this conversation");
    if (!reply || strcmp(reply, "A useful title")) {
        fputs("groktest: ephemeral title turn failed\n", stderr);
        free(reply);
        grok_stop(client);
        return 1;
    }
    free(reply);
    grok_stop(client);
    puts("groktest: ok");
    return 0;
}
