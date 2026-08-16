#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "vendor/agents/claude/claude.h"
#include "vendor/cJSON.h"

static const char *message_text(cJSON *msg)
{
    cJSON *message = cJSON_GetObjectItemCaseSensitive(msg, "message");
    cJSON *content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
    if (cJSON_IsString(content))
        return content->valuestring;
    cJSON *block = cJSON_IsArray(content) ? cJSON_GetArrayItem(content, 0) : NULL;
    return block ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block, "text")) : NULL;
}

static void result(const char *text)
{
    printf("{\"type\":\"result\",\"subtype\":\"success\","
           "\"is_error\":false,\"result\":\"%s\"}\n", text);
    fflush(stdout);
}

static void stray_turn(const char *text)
{
    printf("{\"type\":\"system\",\"subtype\":\"task_notification\"}\n"
           "{\"type\":\"system\",\"subtype\":\"init\",\"model\":\"mock\"}\n");
    fflush(stdout);
    result(text);
}

static void turn(const char *text)
{
    printf("{\"type\":\"system\",\"subtype\":\"init\",\"model\":\"mock\"}\n");
    fflush(stdout);
    result(text);
}

static long milliseconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int has_arg(int argc, char **argv, const char *value)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], value))
            return 1;
    return 0;
}

static int has_option(int argc, char **argv, const char *option, const char *value)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], option) && !strcmp(argv[i + 1], value))
            return 1;
    return 0;
}

static int mock_cli(int argc, char **argv)
{
    if (!has_arg(argc, argv, "--no-session-persistence") ||
        !has_option(argc, argv, "--name", "title helper") ||
        !has_option(argc, argv, "--tools", ""))
        return 2;

    char *line = NULL;
    size_t cap = 0;
    int effort_changes = 0;
    while (getline(&line, &cap, stdin) >= 0) {
        cJSON *msg = cJSON_Parse(line);
        const char *type = msg ? cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(msg, "type")) : NULL;
        cJSON *request = msg ? cJSON_GetObjectItemCaseSensitive(msg, "request") : NULL;
        const char *subtype = request ? cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(request, "subtype")) : NULL;
        if (type && !strcmp(type, "control_request") && subtype &&
            !strcmp(subtype, "initialize")) {
            usleep(400000);
            printf("{\"type\":\"control_response\",\"response\":{"
                   "\"subtype\":\"success\","
                   "\"request_id\":\"claude_h_initialize\","
                   "\"response\":{}},\"session_id\":\"session-1\"}\n");
            fflush(stdout);
            cJSON_Delete(msg);
            continue;
        }
        const char *text = msg ? message_text(msg) : NULL;
        if (text && !strcmp(text, "/effort low") && effort_changes++ == 0)
            result("Set effort level to low");
        else if (text && !strcmp(text, "/effort auto") && effort_changes++ == 1)
            result("Effort level set to auto");
        else if (text && !strcmp(text, "/effort bogus"))
            result("Invalid argument: bogus");
        else if (text && !strcmp(text, "continue"))
            result("done");

        else if (text && !strcmp(text, "race")) {
            stray_turn("background task finished");
            turn("answered");
        }
        else
            result("unexpected input");
        cJSON_Delete(msg);
    }
    free(line);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--print"))
        return mock_cli(argc, argv);

    claude_opts opts = {
        .cli_path = argv[0],
        .session_name = "title helper",
        .tools = "",
        .no_session_persistence = 1,
    };
    long started = milliseconds();
    claude_client *client = claude_start(&opts);
    if (!client) {
        fputs("claudetest: could not start mock CLI\n", stderr);
        return 1;
    }
    if (milliseconds() - started >= 250 || claude_session_id(client)) {
        fputs("claudetest: startup did not return during background prewarm\n", stderr);
        claude_stop(client);
        return 1;
    }
    if (!claude_set_effort(client, "low") ||
        !claude_set_effort(client, NULL) ||
        claude_set_effort(client, "bogus")) {
        fputs("claudetest: live effort command failed\n", stderr);
        claude_stop(client);
        return 1;
    }
    char *reply = claude_send(client, "continue");
    if (!reply || strcmp(reply, "done")) {
        fputs("claudetest: process was not reusable after effort changes\n", stderr);
        free(reply);
        claude_stop(client);
        return 1;
    }
    free(reply);

    claude_result meta = {0};
    reply = claude_send_ex(client, "race", &meta);
    if (!reply || strcmp(reply, "answered")) {
        fprintf(stderr, "claudetest: a stray turn was taken as the reply (%s)\n",
                reply ? reply : "none");
        free(reply);
        claude_stop(client);
        return 1;
    }
    free(reply);
    claude_stop(client);
    puts("claudetest: ok");
    return 0;
}
