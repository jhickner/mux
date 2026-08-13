#include "sessionlist.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "title.h"
#include "vendor/cJSON.h"

#define MAX_SESSIONS 40
/* Transcripts open with mode/permission/snapshot records before any real turn. */
#define SCAN_LINES   40

/* The CLI names a project directory after its cwd with every character outside
 * [A-Za-z0-9-] replaced by a dash. */
static void encode_cwd(const char *cwd, char *out, size_t size)
{
    size_t o = 0;
    for (const char *p = cwd; *p && o + 1 < size; p++) {
        char c = *p;
        int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '-';
        out[o++] = keep ? c : '-';
    }
    out[o] = '\0';
}

static void flatten(const char *in, char *out, size_t size)
{
    size_t o = 0;
    int space = 0;
    for (const char *p = in; *p && o + 1 < size; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (o == 0 || space)
                continue;
            space = 1;
            out[o++] = ' ';
            continue;
        }
        space = 0;
        out[o++] = (char)c;
    }
    while (o > 0 && out[o - 1] == ' ')
        o--;
    out[o] = '\0';
}

static void relative_time(time_t then, char *out, size_t size)
{
    long secs = (long)(time(NULL) - then);
    if (secs < 60)
        snprintf(out, size, "just now");
    else if (secs < 3600)
        snprintf(out, size, "%ldm ago", secs / 60);
    else if (secs < 86400)
        snprintf(out, size, "%ldh ago", secs / 3600);
    else
        snprintf(out, size, "%ldd ago", secs / 86400);
}

/* Slash commands and harness notices are noise as a session label. */
static int usable_label(const char *text)
{
    if (!text || !*text)
        return 0;
    if (text[0] == '<' || text[0] == '/')
        return 0;
    return strncmp(text, "Caveat:", 7) != 0;
}

/* Pull a label out of a transcript: the name Claude Code gave the conversation
 * if it wrote one, otherwise the first genuine user message. */
static int transcript_label(const char *path, char *out, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int scanned = 0, titled = 0, spoke = 0;

    while (!titled && scanned++ < SCAN_LINES && (n = getline(&line, &cap, f)) > 0) {
        cJSON *ev = cJSON_ParseWithLength(line, (size_t)n);
        if (!ev)
            continue;
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "type"));
        cJSON *meta = cJSON_GetObjectItem(ev, "isMeta");
        /* The CLI names the conversation just after its first turn, so the title
         * lands a few records below the message that would stand in for it. */
        if (type && strcmp(type, "ai-title") == 0) {
            const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "aiTitle"));
            if (title && *title) {
                flatten(title, out, size);
                titled = 1;
            }
        } else if (!spoke && type && strcmp(type, "user") == 0 && !cJSON_IsTrue(meta)) {
            cJSON *message = cJSON_GetObjectItem(ev, "message");
            cJSON *content = message ? cJSON_GetObjectItem(message, "content") : NULL;
            const char *text = NULL;
            if (cJSON_IsString(content)) {
                text = content->valuestring;
            } else if (cJSON_IsArray(content)) {
                cJSON *block;
                cJSON_ArrayForEach(block, content) {
                    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(block, "text"));
                    if (t) {
                        text = t;
                        break;
                    }
                }
            }
            if (usable_label(text)) {
                flatten(text, out, size);
                spoke = 1;
            }
        }
        cJSON_Delete(ev);
    }
    free(line);
    fclose(f);
    return titled || spoke;
}

static int by_recency(const void *a, const void *b)
{
    const struct past_session *x = a, *y = b;
    if (x->modified == y->modified)
        return 0;
    return x->modified < y->modified ? 1 : -1;
}

/* Keep the newest MAX_SESSIONS without depending on readdir() order. */
static void keep_recent(struct past_session *list, int *count,
                        const struct past_session *candidate)
{
    if (*count < MAX_SESSIONS) {
        list[(*count)++] = *candidate;
        return;
    }

    int oldest = 0;
    for (int i = 1; i < *count; i++)
        if (list[i].modified < list[oldest].modified)
            oldest = i;
    if (candidate->modified > list[oldest].modified)
        list[oldest] = *candidate;
}

static int finish_list(struct past_session *list, int count, struct past_session **out)
{
    if (count == 0) {
        free(list);
        return 0;
    }
    qsort(list, (size_t)count, sizeof *list, by_recency);
    *out = list;
    return count;
}

static int load_claude(const char *cwd, const char *skip_id, struct past_session **out)
{
    const char *home = getenv("HOME");
    if (!home || !cwd)
        return 0;

    char encoded[1024];
    encode_cwd(cwd, encoded, sizeof encoded);

    char dir[2048];
    snprintf(dir, sizeof dir, "%s/.claude/projects/%s", home, encoded);
    DIR *d = opendir(dir);
    if (!d)
        return 0;

    struct past_session *list = calloc(MAX_SESSIONS, sizeof *list);
    if (!list) {
        closedir(d);
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        size_t len = strlen(entry->d_name);
        if (len < 7 || strcmp(entry->d_name + len - 6, ".jsonl") != 0)
            continue;

        char id[128];
        size_t id_len = len - 6;
        if (id_len >= sizeof id)
            continue;
        memcpy(id, entry->d_name, id_len);
        id[id_len] = '\0';
        if (skip_id && strcmp(id, skip_id) == 0)
            continue;

        char path[3072];
        snprintf(path, sizeof path, "%s/%s", dir, entry->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || st.st_size == 0)
            continue;

        struct past_session candidate = {0};
        struct past_session *s = &candidate;
        /* Our own title first: the CLI only names the sessions it drives itself. */
        if (!title_lookup(id, s->label, sizeof s->label) &&
            !transcript_label(path, s->label, sizeof s->label))
            continue;
        snprintf(s->id, sizeof s->id, "%s", id);
        s->modified = st.st_mtime;
        relative_time(s->modified, s->when, sizeof s->when);
        keep_recent(list, &count, s);
    }
    closedir(d);
    return finish_list(list, count, out);
}

/* Grok URL-encodes the cwd; when that name would exceed 255 bytes it uses a
 * slug and writes the original path in a .cwd file inside the group. */
static void encode_cwd_grok(const char *cwd, char *out, size_t size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)cwd; *p && o + 1 < size; p++) {
        unsigned char c = *p;
        int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (keep) {
            out[o++] = (char)c;
        } else if (o + 3 < size) {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        } else {
            break;
        }
    }
    out[o] = '\0';
}

static int grok_home(char *out, size_t size)
{
    const char *env = getenv("GROK_HOME");
    if (env && *env) {
        snprintf(out, size, "%s", env);
        return 1;
    }
    const char *home = getenv("HOME");
    if (!home)
        return 0;
    snprintf(out, size, "%s/.grok", home);
    return 1;
}

static int grok_group_dir(const char *cwd, char *out, size_t size)
{
    char home[1024], encoded[2048];
    if (!grok_home(home, sizeof home) || !cwd)
        return 0;
    encode_cwd_grok(cwd, encoded, sizeof encoded);

    snprintf(out, size, "%s/sessions/%s", home, encoded);
    struct stat st;
    if (stat(out, &st) == 0 && S_ISDIR(st.st_mode))
        return 1;

    char sessions[1200];
    snprintf(sessions, sizeof sessions, "%s/sessions", home);
    DIR *d = opendir(sessions);
    if (!d)
        return 0;
    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (entry->d_name[0] == '.')
            continue;
        char marker[3200];
        snprintf(marker, sizeof marker, "%s/%s/.cwd", sessions, entry->d_name);
        FILE *f = fopen(marker, "r");
        if (!f)
            continue;
        char line[4096];
        int match = 0;
        if (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = '\0';
            match = strcmp(line, cwd) == 0;
        }
        fclose(f);
        if (match) {
            snprintf(out, size, "%s/%s", sessions, entry->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static int grok_label(const char *dir, char *out, size_t size)
{
    char path[3072];
    snprintf(path, sizeof path, "%s/summary.json", dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    long n = ftell(f);
    if (n <= 0 || n > 1 << 20) {
        fclose(f);
        return 0;
    }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';

    int ok = 0;
    cJSON *root = cJSON_ParseWithLength(buf, got);
    free(buf);
    if (!root)
        return 0;
    const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(root, "generated_title"));
    if (!title || !*title)
        title = cJSON_GetStringValue(cJSON_GetObjectItem(root, "session_summary"));
    if (title && *title) {
        flatten(title, out, size);
        ok = out[0] != '\0';
    }
    cJSON_Delete(root);
    return ok;
}

static int load_grok(const char *cwd, const char *skip_id, struct past_session **out)
{
    char dir[2048];
    if (!grok_group_dir(cwd, dir, sizeof dir))
        return 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;

    struct past_session *list = calloc(MAX_SESSIONS, sizeof *list);
    if (!list) {
        closedir(d);
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (entry->d_name[0] == '.')
            continue;
        if (strlen(entry->d_name) >= sizeof list[0].id)
            continue;
        if (skip_id && strcmp(entry->d_name, skip_id) == 0)
            continue;

        char path[3072];
        snprintf(path, sizeof path, "%s/%s", dir, entry->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        struct past_session candidate = {0};
        struct past_session *s = &candidate;
        if (!title_lookup(entry->d_name, s->label, sizeof s->label) &&
            !grok_label(path, s->label, sizeof s->label))
            continue;
        snprintf(s->id, sizeof s->id, "%s", entry->d_name);
        s->modified = st.st_mtime;
        relative_time(s->modified, s->when, sizeof s->when);
        keep_recent(list, &count, s);
    }
    closedir(d);
    return finish_list(list, count, out);
}

int sessionlist_available(const char *backend)
{
    return backend && (!strcmp(backend, "claude") || !strcmp(backend, "grok"));
}

int sessionlist_load(const char *backend, const char *cwd, const char *skip_id,
                     struct past_session **out)
{
    *out = NULL;
    if (!backend || !strcmp(backend, "claude"))
        return load_claude(cwd, skip_id, out);
    if (!strcmp(backend, "grok"))
        return load_grok(cwd, skip_id, out);
    return 0;
}
