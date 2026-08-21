#include "sessionload.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "md.h"
#include "prompt.h"
#include "sessionlist.h"
#include "session.h"
#include "sessionview.h"
#include "ui.h"
#include "vendor/agents/backend.h"
#include "vendor/cJSON.h"

// The three CLIs that keep a transcript all write one JSON object per line with
// a role and a list of content blocks; where they differ is only in what the
// line is called and where the message sits inside it.

#define TURNS_MAX  400      /* how far back a replayed conversation goes */
#define LINE_MAX   (4 << 20)

int sessionload_available(const char *backend)
{
    return backend && (!strcmp(backend, "claude") || !strcmp(backend, "grok") ||
                       !strcmp(backend, "pi"));
}

static int is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// pi names its transcripts for the time they started, so the id is inside.
static int pi_find(const char *dir, const char *id, char *out, size_t size)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;

    int found = 0;
    struct dirent *e;
    while (!found && (e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 7 || strcmp(e->d_name + len - 6, ".jsonl") != 0)
            continue;
        if (!strstr(e->d_name, id))
            continue;
        if ((size_t)snprintf(out, size, "%s/%s", dir, e->d_name) < size)
            found = is_file(out);
    }
    closedir(d);
    return found;
}

int sessionload_path(const char *backend, const char *cwd, const char *id,
                     char *out, size_t size)
{
    char dir[2048];
    if (!id || !*id || strchr(id, '/') || !sessionload_available(backend))
        return 0;
    if (!sessionlist_dir(backend, cwd, dir, sizeof dir))
        return 0;

    if (!strcmp(backend, "grok"))
        return (size_t)snprintf(out, size, "%s/%s/chat_history.jsonl", dir, id) < size &&
               is_file(out);
    if (!strcmp(backend, "pi"))
        return pi_find(dir, id, out, size);
    return (size_t)snprintf(out, size, "%s/%s.jsonl", dir, id) < size && is_file(out);
}

/* --- what a line says ---------------------------------------------------- */

enum role {
    ROLE_NONE,
    ROLE_USER,
    ROLE_ASSISTANT,
    ROLE_THINKING,
};

static enum role role_of(const char *name)
{
    if (!name)
        return ROLE_NONE;
    if (!strcmp(name, "user"))
        return ROLE_USER;
    if (!strcmp(name, "assistant"))
        return ROLE_ASSISTANT;
    if (!strcmp(name, "reasoning") || !strcmp(name, "thinking"))
        return ROLE_THINKING;
    return ROLE_NONE;
}

// The message, and what it was: pi wraps every one in a "message" line, claude
// names the line for the role, grok puts the role at the top level.
static enum role line_message(const cJSON *ev, const cJSON **content)
{
    *content = NULL;
    if (cJSON_IsTrue(cJSON_GetObjectItem(ev, "isMeta")) ||
        cJSON_IsTrue(cJSON_GetObjectItem(ev, "isSidechain")))
        return ROLE_NONE;

    const cJSON *message = cJSON_GetObjectItem(ev, "message");
    const cJSON *body = message ? message : ev;
    const char  *role = cJSON_GetStringValue(cJSON_GetObjectItem(body, "role"));
    if (!role)
        role = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "type"));

    *content = cJSON_GetObjectItem(body, "content");
    return *content ? role_of(role) : ROLE_NONE;
}

// What the user actually typed. The CLIs wrap it in context of their own —
// grok in a <user_query>, all of them in system reminders — and none of that
// was ever on screen.
static int user_text(const char *text, char *out, size_t size)
{
    if (!text || !*text)
        return 0;

    const char *open = strstr(text, "<user_query>");
    const char *end = NULL;
    if (open) {
        text = open + strlen("<user_query>");
        end = strstr(text, "</user_query>");
    }

    size_t n = end ? (size_t)(end - text) : strlen(text);
    while (n && (text[n - 1] == '\n' || text[n - 1] == ' '))
        n--;
    while (n && (*text == '\n' || *text == ' ')) {
        text++;
        n--;
    }
    if (!n || n >= size)
        n = n >= size ? size - 1 : n;
    if (!n)
        return 0;

    memcpy(out, text, n);
    out[n] = '\0';
    if (!strncmp(out, "<system-reminder>", 17) || !strncmp(out, "<user_info>", 11) ||
        !strncmp(out, "Caveat:", 7) || out[0] == '<')
        return 0;
    return 1;
}

static void draw_tool(const cJSON *block, const char *cwd)
{
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(block, "name"));
    if (!name)
        return;

    cJSON *input = cJSON_GetObjectItem(block, "input");
    char  *json = input ? cJSON_PrintUnformatted(input) : NULL;

    backend_event ev = {0};
    ev.kind = BACKEND_EV_TOOL;
    ev.name = name;
    ev.input_json = json;

    char arg[4096];
    view_tool_argument(&ev, cwd, arg, sizeof arg);
    view_keep_tool_call(name, arg, 0);
    free(json);
}

// One message, drawn the way the session itself would have drawn it. Tool
// results are left out: the call above them says what ran, and the output was
// only ever worth the room while it was live.
static int draw_message(enum role role, const cJSON *content, const char *cwd,
                        int thinking)
{
    char text[8192];
    int  drew = 0;

    if (cJSON_IsString(content)) {
        if (role == ROLE_USER && user_text(content->valuestring, text, sizeof text)) {
            prompt_echo_message(text);
            return 1;
        }
        if (role == ROLE_ASSISTANT && *content->valuestring) {
            md_render_kept(content->valuestring, 0);
            return 1;
        }
        if (role == ROLE_THINKING && thinking && *content->valuestring) {
            view_keep_activity("\xe2\x9c\xbb", content->valuestring, UI_THINKING, 0);
            return 1;
        }
        return 0;
    }

    const cJSON *block;
    cJSON_ArrayForEach(block, content) {
        const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
        const char *body = cJSON_GetStringValue(cJSON_GetObjectItem(block, "text"));
        if (!kind)
            continue;

        if (!strcmp(kind, "text") && body) {
            if (role == ROLE_USER) {
                if (user_text(body, text, sizeof text)) {
                    prompt_echo_message(text);
                    drew = 1;
                }
            } else if (*body) {
                md_render_kept(body, 0);
                drew = 1;
            }
        } else if (!strcmp(kind, "thinking") || !strcmp(kind, "reasoning")) {
            const char *reason = body ? body
                                      : cJSON_GetStringValue(
                                            cJSON_GetObjectItem(block, "thinking"));
            if (thinking && reason && *reason) {
                view_keep_activity("\xe2\x9c\xbb", reason, UI_THINKING, 0);
                drew = 1;
            }
        } else if (!strcmp(kind, "tool_use")) {
            draw_tool(block, cwd);
            drew = 1;
        }
    }
    return drew;
}

// Counted first so a long conversation comes back from where it still fits,
// rather than from a beginning nobody asked for.
static int count_turns(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    char   *line = NULL;
    size_t  cap = 0;
    ssize_t n;
    int     turns = 0;

    while ((n = getline(&line, &cap, f)) > 0) {
        if (n > LINE_MAX)
            continue;
        cJSON *ev = cJSON_ParseWithLength(line, (size_t)n);
        if (!ev)
            continue;
        const cJSON *content = NULL;
        if (line_message(ev, &content) != ROLE_NONE)
            turns++;
        cJSON_Delete(ev);
    }
    free(line);
    fclose(f);
    return turns;
}

int sessionload_into(const struct session *s)
{
    const char *id = s ? session_id(s) : NULL;
    if (!id)
        return 0;
    return sessionload_replay(session_backend(s), session_cwd(s), id,
                              session_thinking(s));
}

int sessionload_replay(const char *backend, const char *cwd, const char *id,
                       int thinking)
{
    char path[4096];
    if (!sessionload_path(backend, cwd, id, path, sizeof path))
        return 0;

    int turns = count_turns(path);
    int skip = turns > TURNS_MAX ? turns - TURNS_MAX : 0;

    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    if (skip) {
        ui_bar(ui_style(UI_DIM), "\xe2\x80\xa6 %d earlier turns not shown", skip);
        ui_put("\n");
    }

    char   *line = NULL;
    size_t  cap = 0;
    ssize_t n;
    int     seen = 0, drawn = 0;

    while ((n = getline(&line, &cap, f)) > 0) {
        if (n > LINE_MAX)
            continue;
        cJSON *ev = cJSON_ParseWithLength(line, (size_t)n);
        if (!ev)
            continue;

        const cJSON *content = NULL;
        enum role role = line_message(ev, &content);
        if (role != ROLE_NONE && seen++ >= skip)
            drawn += draw_message(role, content, cwd, thinking);
        cJSON_Delete(ev);
    }
    free(line);
    fclose(f);
    ui_flush();
    return drawn;
}
