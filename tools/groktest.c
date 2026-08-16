#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/agents/grok/grok.h"
#include "vendor/cJSON.h"

static int tool_starts, tool_results, tool_fails;
static char last_result[256];
static char last_tool[64];
static char last_input[256];
static int last_failed;

static void on_event(void *ud, const grok_event *ev)
{
    (void)ud;
    if (ev->kind == GROK_EV_TOOL) {
        tool_starts++;
        snprintf(last_tool, sizeof last_tool, "%s", ev->name ? ev->name : "");
        snprintf(last_input, sizeof last_input, "%s", ev->input_json ? ev->input_json : "");
    }
    if (ev->kind == GROK_EV_TOOL_RESULT) {
        tool_results++;
        last_failed = ev->failed;
        snprintf(last_result, sizeof last_result, "%s", ev->text ? ev->text : "");
        if (ev->failed)
            tool_fails++;
    }
}

static void respond(int id, const char *result)
{
    printf("{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}\n", id, result);
    fflush(stdout);
}

static const char *prompt_text(cJSON *msg)
{
    cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
    cJSON *prompt = params ? cJSON_GetObjectItemCaseSensitive(params, "prompt") : NULL;
    cJSON *block = cJSON_IsArray(prompt) ? cJSON_GetArrayItem(prompt, 0) : NULL;
    return block ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block, "text")) : NULL;
}

static void emit_tool(const char *id, const char *status, const char *content,
                      const char *raw)
{
    printf("{\"jsonrpc\":\"2.0\",\"method\":\"session/update\","
           "\"params\":{\"update\":{\"sessionUpdate\":\"tool_call\","
           "\"toolCallId\":\"%s\",\"kind\":\"edit\",\"title\":\"status.c\","
           "\"status\":\"in_progress\","
           "\"locations\":[{\"path\":\"/tmp/status.c\"}]}}}\n", id);
    printf("{\"jsonrpc\":\"2.0\",\"method\":\"session/update\","
           "\"params\":{\"update\":{\"sessionUpdate\":\"tool_call_update\","
           "\"toolCallId\":\"%s\",\"status\":\"%s\",\"content\":%s",
           id, status, content);
    if (raw)
        printf(",\"rawOutput\":%s", raw);
    printf("}}}\n");
    fflush(stdout);
}

static int pinned_model;

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
        } else if (method && !strcmp(method, "session/set_model")) {
            cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
            const char *want = params ? cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(params, "modelId")) : NULL;
            if (want && !strcmp(want, "grok-test")) {
                pinned_model = 1;
                respond(id, "{}");
            } else {
                printf("{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{"
                       "\"code\":-32602,\"message\":\"unknown model\"}}\n", id);
                fflush(stdout);
            }
        } else if (method && !strcmp(method, "session/set_mode")) {
            respond(id, "{}");
        } else if (method && !strcmp(method, "session/prompt")) {
            const char *text = prompt_text(msg);
            if (text && strstr(text, "path-only")) {
                emit_tool("edit-path", "failed",
                          "[{\"type\":\"diff\",\"path\":\"/tmp/status.c\"}]", NULL);
            } else if (text && strstr(text, "denied")) {
                emit_tool("edit-msg", "failed",
                          "[{\"type\":\"content\",\"content\":{\"type\":\"text\","
                          "\"text\":\"Permission denied\"}}]", NULL);
            } else if (text && strstr(text, "not-found")) {
                emit_tool("edit-var", "failed", "[]",
                          "{\"type\":\"NotFound\",\"NotFound\":\"no such file\"}");
            } else if (text && strstr(text, "read-error")) {
                emit_tool("edit-nest", "failed", "[]",
                          "{\"type\":\"FileReadError\","
                          "\"FileReadError\":{\"message\":\"cannot read status.c\"}}");
            } else if (text && strstr(text, "split-reason")) {
                printf("{\"jsonrpc\":\"2.0\",\"method\":\"session/update\","
                       "\"params\":{\"update\":{\"sessionUpdate\":\"tool_call\","
                       "\"toolCallId\":\"edit-split\",\"kind\":\"edit\","
                       "\"title\":\"status.c\",\"status\":\"in_progress\","
                       "\"rawOutput\":{\"message\":\"Permission denied\"}}}}\n");
                printf("{\"jsonrpc\":\"2.0\",\"method\":\"session/update\","
                       "\"params\":{\"update\":{\"sessionUpdate\":"
                       "\"tool_call_update\",\"toolCallId\":\"edit-split\","
                       "\"status\":\"failed\"}}}\n");
                fflush(stdout);
            } else if (text && strstr(text, "type-only")) {
                emit_tool("edit-type", "failed", "[]",
                          "{\"type\":\"PermissionDenied\"}");
            } else if (text && strstr(text, "web-search")) {
                printf("{\"jsonrpc\":\"2.0\",\"method\":\"session/update\","
                       "\"params\":{\"update\":{\"sessionUpdate\":\"tool_call\","
                       "\"toolCallId\":\"ws-1\",\"title\":\"Web search:\","
                       "\"kind\":\"search\",\"status\":\"in_progress\","
                       "\"rawInput\":{\"variant\":\"WebSearch\","
                       "\"backend\":true}}}}\n");
                printf("{\"jsonrpc\":\"2.0\",\"method\":\"session/update\","
                       "\"params\":{\"update\":{\"sessionUpdate\":"
                       "\"tool_call_update\",\"toolCallId\":\"ws-1\","
                       "\"status\":\"completed\",\"title\":\"Web search:\","
                       "\"rawOutput\":{\"action\":{\"type\":\"search\","
                       "\"query\":\"COLMAP Apple Silicon\"}}}}}\n");
                fflush(stdout);
            } else if (text && strstr(text, "grep-late")) {
                printf("{\"jsonrpc\":\"2.0\",\"method\":\"session/update\","
                       "\"params\":{\"update\":{\"sessionUpdate\":\"tool_call\","
                       "\"toolCallId\":\"grep-1\",\"title\":\"grep\","
                       "\"_meta\":{\"x.ai/tool\":{\"name\":\"grep\","
                       "\"kind\":\"search\"}}}}}\n");
                printf("{\"jsonrpc\":\"2.0\",\"method\":\"session/update\","
                       "\"params\":{\"update\":{\"sessionUpdate\":"
                       "\"tool_call_update\",\"toolCallId\":\"grep-1\","
                       "\"kind\":\"search\",\"title\":\"monthly|weekly\","
                       "\"rawInput\":{\"variant\":\"Grep\","
                       "\"pattern\":\"monthly|weekly\"}}}}\n");
                fflush(stdout);
            } else {
                printf("{\"jsonrpc\":\"2.0\",\"method\":\"session/update\","
                       "\"params\":{\"update\":{\"sessionUpdate\":"
                       "\"agent_message_chunk\",\"content\":{\"type\":\"text\","
                       "\"text\":\"%s\"}}}}\n",
                       pinned_model ? "A useful title" : "model was not pinned");
            }
            respond(id, "{\"stopReason\":\"end_turn\"}");
        } else if (idj) {
            /* An unanswered request leaves the client blocked forever, so
             * anything unrecognised gets an error rather than silence. */
            printf("{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{"
                   "\"code\":-32601,\"message\":\"unhandled method: %s\"}}\n",
                   id, method ? method : "(none)");
            fflush(stdout);
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
    grok_set_event_cb(client, on_event, NULL);

    char *reply = grok_send(client, "name this conversation");
    if (!reply || strcmp(reply, "A useful title")) {
        fputs("groktest: ephemeral title turn failed\n", stderr);
        free(reply);
        grok_stop(client);
        return 1;
    }
    free(reply);

    reply = grok_send(client, "path-only edit");
    free(reply);
    if (tool_starts != 1 || tool_results != 1 || tool_fails != 1 ||
        !last_failed || strcmp(last_result, "failed")) {
        fprintf(stderr, "groktest: path-only failure was not surfaced "
                "(starts=%d results=%d fails=%d failed=%d text=%s)\n",
                tool_starts, tool_results, tool_fails, last_failed, last_result);
        grok_stop(client);
        return 1;
    }

    reply = grok_send(client, "denied edit");
    free(reply);
    if (tool_starts != 2 || tool_results != 2 || tool_fails != 2 ||
        !last_failed || strcmp(last_result, "Permission denied")) {
        fprintf(stderr, "groktest: error text was not preserved "
                "(starts=%d results=%d fails=%d failed=%d text=%s)\n",
                tool_starts, tool_results, tool_fails, last_failed, last_result);
        grok_stop(client);
        return 1;
    }

    reply = grok_send(client, "not-found edit");
    free(reply);
    if (!last_failed || strcmp(last_result, "no such file")) {
        fprintf(stderr, "groktest: variant rawOutput was not mined (%s)\n",
                last_result);
        grok_stop(client);
        return 1;
    }

    reply = grok_send(client, "read-error edit");
    free(reply);
    if (!last_failed || strcmp(last_result, "cannot read status.c")) {
        fprintf(stderr, "groktest: nested rawOutput message was not mined (%s)\n",
                last_result);
        grok_stop(client);
        return 1;
    }

    reply = grok_send(client, "split-reason edit");
    free(reply);
    if (!last_failed || strcmp(last_result, "Permission denied")) {
        fprintf(stderr, "groktest: earlier rawOutput was dropped on status-only "
                "update (%s)\n", last_result);
        grok_stop(client);
        return 1;
    }

    reply = grok_send(client, "type-only edit");
    free(reply);
    if (!last_failed || strcmp(last_result, "PermissionDenied")) {
        fprintf(stderr, "groktest: type-only rawOutput was dropped (%s)\n",
                last_result);
        grok_stop(client);
        return 1;
    }

    int starts = tool_starts;
    reply = grok_send(client, "web-search");
    free(reply);
    if (tool_starts != starts + 1 || strcmp(last_tool, "web") ||
        !strstr(last_input, "COLMAP Apple Silicon")) {
        fprintf(stderr, "groktest: web search was not named/queried "
                "(starts=%d tool=%s input=%s)\n",
                tool_starts - starts, last_tool, last_input);
        grok_stop(client);
        return 1;
    }

    starts = tool_starts;
    reply = grok_send(client, "grep-late");
    free(reply);
    if (tool_starts != starts + 1 || strcmp(last_tool, "grep") ||
        !strstr(last_input, "monthly|weekly")) {
        fprintf(stderr, "groktest: late grep pattern was dropped "
                "(starts=%d tool=%s input=%s)\n",
                tool_starts - starts, last_tool, last_input);
        grok_stop(client);
        return 1;
    }

    grok_stop(client);
    puts("groktest: ok");
    return 0;
}
