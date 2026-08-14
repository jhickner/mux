#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SETTINGS 64
#define MAX_KEY      64
#define MAX_VALUE    128

static struct {
    char key[MAX_KEY];
    char value[MAX_VALUE];
} entries[MAX_SETTINGS];

static int  count;
static char file_path[4096];

static int find(const char *key)
{
    for (int i = 0; i < count; i++)
        if (strcmp(entries[i].key, key) == 0)
            return i;
    return -1;
}

void settings_open(const char *path)
{
    count = 0;
    snprintf(file_path, sizeof file_path, "%s", path ? path : "");
    if (!file_path[0])
        return;

    FILE *f = fopen(file_path, "r");
    if (!f)
        return;

    char line[256];
    while (count < MAX_SETTINGS && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        size_t n = strlen(line);
        if (n == 0 || n >= MAX_KEY)
            continue;
        char *value = eq + 1;
        value[strcspn(value, "\r\n")] = '\0';
        snprintf(entries[count].key, MAX_KEY, "%s", line);
        snprintf(entries[count].value, MAX_VALUE, "%s", value);
        count++;
    }
    fclose(f);
}

const char *settings_get_str(const char *key, const char *fallback)
{
    int i = find(key);
    return i < 0 ? fallback : entries[i].value;
}

void settings_set_str(const char *key, const char *value)
{
    if (!value)
        return;
    int i = find(key);
    if (i < 0) {
        if (!file_path[0] || count >= MAX_SETTINGS || strlen(key) >= MAX_KEY)
            return;
        i = count++;
        snprintf(entries[i].key, MAX_KEY, "%s", key);
    } else if (strcmp(entries[i].value, value) == 0) {
        return;
    }
    snprintf(entries[i].value, MAX_VALUE, "%s", value);

    FILE *f = fopen(file_path, "w");
    if (!f)
        return;
    for (int j = 0; j < count; j++)
        fprintf(f, "%s=%s\n", entries[j].key, entries[j].value);
    fclose(f);
}

int settings_get_int(const char *key, int fallback)
{
    int i = find(key);
    return i < 0 ? fallback : atoi(entries[i].value);
}

void settings_set_int(const char *key, int value)
{
    char text[16];
    snprintf(text, sizeof text, "%d", value);
    settings_set_str(key, text);
}
