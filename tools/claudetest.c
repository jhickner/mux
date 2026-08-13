#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int mock_cli(void)
{
    char *line = NULL;
    size_t cap = 0;
    int effort_changes = 0;
    while (getline(&line, &cap, stdin) >= 0) {
        cJSON *msg = cJSON_Parse(line);
        const char *text = msg ? message_text(msg) : NULL;
        if (text && !strcmp(text, "/effort low") && effort_changes++ == 0)
            result("Set effort level to low");
        else if (text && !strcmp(text, "/effort auto") && effort_changes++ == 1)
            result("Effort level set to auto");
        else if (text && !strcmp(text, "/effort bogus"))
            result("Invalid argument: bogus");
        else if (text && !strcmp(text, "continue"))
            result("done");
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
        return mock_cli();

    claude_opts opts = { .cli_path = argv[0] };
    claude_client *client = claude_start(&opts);
    if (!client) {
        fputs("claudetest: could not start mock CLI\n", stderr);
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
    claude_stop(client);
    puts("claudetest: ok");
    return 0;
}
