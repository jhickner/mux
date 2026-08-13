#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/agents/pi/pi.h"
#include "vendor/cJSON.h"

static int abort_turn;

static int should_abort(void)
{
    return abort_turn;
}

static void respond(const char *id, const char *command)
{
    printf("{\"id\":\"%s\",\"type\":\"response\","
           "\"command\":\"%s\",\"success\":true}\n", id, command);
    fflush(stdout);
}

static int mock_server(void)
{
    char *line = NULL;
    size_t cap = 0;
    int turns = 0;

    while (getline(&line, &cap, stdin) >= 0) {
        cJSON *msg = cJSON_Parse(line);
        const char *id = msg ? cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(msg, "id")) : NULL;
        const char *type = msg ? cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(msg, "type")) : NULL;

        if (id && type && !strcmp(type, "prompt")) {
            turns++;
            respond(id, "prompt");
            printf("{\"type\":\"message_update\","
                   "\"assistantMessageEvent\":{\"type\":\"text_delta\","
                   "\"delta\":\"%s\"}}\n",
                   turns == 1 ? "partial" : "done");
            if (turns > 1)
                printf("{\"type\":\"agent_settled\"}\n");
            fflush(stdout);
        } else if (id && type && !strcmp(type, "abort")) {
            respond(id, "abort");
            printf("{\"type\":\"agent_settled\"}\n");
            fflush(stdout);
        }
        cJSON_Delete(msg);
    }
    free(line);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--mode"))
        return mock_server();

    pi_opts opts = { .cli_path = argv[0], .no_session = 1 };
    pi_client *client = pi_start(&opts);
    if (!client) {
        fprintf(stderr, "pitest: could not start mock RPC process\n");
        return 1;
    }

    pi_set_abort_check(client, should_abort);
    abort_turn = 1;
    pi_result meta = {0};
    char *reply = pi_send_ex(client, "interrupt me", &meta);
    if (!reply || strcmp(reply, "partial") || !meta.interrupted) {
        fprintf(stderr, "pitest: interrupted operation was reported as a failure\n");
        free(reply);
        pi_stop(client);
        return 1;
    }
    free(reply);

    abort_turn = 0;
    reply = pi_send(client, "continue");
    if (!reply || strcmp(reply, "done")) {
        fprintf(stderr, "pitest: process was not reusable after abort\n");
        free(reply);
        pi_stop(client);
        return 1;
    }
    free(reply);
    pi_stop(client);
    puts("pitest: ok");
    return 0;
}
