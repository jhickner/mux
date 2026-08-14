#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/agents/pi/pi.h"
#include "vendor/cJSON.h"

static int abort_turn;
static int tool_starts, tool_ends;
static char tool_name[64], tool_input[256], tool_output[256];

static int should_abort(void)
{
    return abort_turn;
}

static void capture_event(void *ud, const pi_event *ev)
{
    (void)ud;
    if (ev->kind == PI_EV_TOOL) {
        tool_starts++;
        snprintf(tool_name, sizeof tool_name, "%s", ev->name ? ev->name : "");
        snprintf(tool_input, sizeof tool_input, "%s",
                 ev->input_json ? ev->input_json : "");
    } else if (ev->kind == PI_EV_TOOL_RESULT) {
        tool_ends++;
        snprintf(tool_output, sizeof tool_output, "%s", ev->text ? ev->text : "");
    }
}

static void respond(const char *id, const char *command)
{
    printf("{\"id\":\"%s\",\"type\":\"response\","
           "\"command\":\"%s\",\"success\":true}\n", id, command);
    fflush(stdout);
}

static int mock_server(int argc, char **argv)
{
    int startup_effort = 0, no_session = 0, no_tools = 0;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--thinking") && !strcmp(argv[i + 1], "high"))
            startup_effort = 1;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--no-session"))
            no_session = 1;
        else if (!strcmp(argv[i], "--no-tools"))
            no_tools = 1;
    if (!startup_effort || !no_session || !no_tools)
        return 2;

    char *line = NULL;
    size_t cap = 0;
    int turns = 0, effort_changes = 0;

    while (getline(&line, &cap, stdin) >= 0) {
        cJSON *msg = cJSON_Parse(line);
        const char *id = msg ? cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(msg, "id")) : NULL;
        const char *type = msg ? cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(msg, "type")) : NULL;

        if (id && type && !strcmp(type, "get_state")) {
            printf("{\"id\":\"%s\",\"type\":\"response\","
                   "\"command\":\"get_state\",\"success\":true,"
                   "\"data\":{\"thinkingLevel\":\"medium\"}}\n", id);
            fflush(stdout);
        } else if (id && type && !strcmp(type, "set_thinking_level")) {
            const char *level = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(msg, "level"));
            const char *expected = effort_changes++ == 0 ? "low" : "medium";
            if (level && !strcmp(level, expected))
                respond(id, "set_thinking_level");
            else {
                printf("{\"id\":\"%s\",\"type\":\"response\","
                       "\"command\":\"set_thinking_level\",\"success\":false}\n", id);
                fflush(stdout);
            }
        } else if (id && type && !strcmp(type, "prompt")) {
            turns++;
            respond(id, "prompt");
            printf("{\"type\":\"message_update\","
                   "\"assistantMessageEvent\":{\"type\":\"text_delta\","
                   "\"delta\":\"%s\"}}\n",
                   turns == 1 ? "partial" : "done");
            if (turns > 1) {
                printf("{\"type\":\"tool_execution_start\","
                       "\"toolCallId\":\"tool-1\",\"toolName\":\"edit\","
                       "\"args\":{\"path\":\"src/session.c\","
                       "\"oldText\":\"old\",\"newText\":\"new\"}}\n");
                printf("{\"type\":\"tool_execution_end\","
                       "\"toolCallId\":\"tool-1\",\"toolName\":\"edit\","
                       "\"result\":{\"content\":[{\"type\":\"text\","
                       "\"text\":\"Successfully replaced 1 block(s)\"}]},"
                       "\"isError\":false}\n");
                printf("{\"type\":\"agent_settled\"}\n");
            }
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
        return mock_server(argc, argv);

    pi_opts opts = {
        .cli_path = argv[0], .effort = "high", .no_session = 1, .no_tools = 1,
    };
    pi_client *client = pi_start(&opts);
    if (!client) {
        fprintf(stderr, "pitest: could not start mock RPC process\n");
        return 1;
    }

    pi_set_event_cb(client, capture_event, NULL);
    if (!pi_set_effort(client, "low")) {
        fprintf(stderr, "pitest: could not change live thinking effort\n");
        pi_stop(client);
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
    if (tool_starts != 1 || tool_ends != 1 || strcmp(tool_name, "edit") ||
        strcmp(tool_input,
               "{\"path\":\"src/session.c\",\"oldText\":\"old\",\"newText\":\"new\"}") ||
        strcmp(tool_output, "Successfully replaced 1 block(s)")) {
        fprintf(stderr, "pitest: structured tool event was not preserved\n");
        pi_stop(client);
        return 1;
    }
    if (!pi_set_effort(client, NULL)) {
        fprintf(stderr, "pitest: could not restore default thinking effort\n");
        pi_stop(client);
        return 1;
    }
    pi_stop(client);
    puts("pitest: ok");
    return 0;
}
