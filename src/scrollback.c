#include "scrollback.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bash.h"
#include "image.h"
#include "md.h"
#include "prompt.h"
#include "sessionview.h"
#include "sidechannel.h"
#include "viewport.h"
#include "vendor/cJSON.h"

// Every entry that can be rebuilt live. What is not here comes back as rows.
static const struct {
    const char *kind;
    void      (*load)(const cJSON *st);
} LOADERS[] = {
    {MD_KEPT_KIND,        md_kept_load},
    {VIEW_KEEP_KIND,      view_keep_load},
    {PROMPT_ECHO_KIND,    prompt_echo_load},
    {BASH_RAN_KIND,       bash_ran_load},
    {SIDECHANNEL_BTW_KIND, sidechannel_btw_load},
    {IMAGE_PLACED_KIND,   image_placed_load},
};

static void load_rows(const cJSON *rows)
{
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        const char *s = cJSON_GetStringValue(row);
        if (!s)
            continue;
        viewport_write(s, strlen(s));
        viewport_write("\n", 1);
    }
}

static void load_line(const char *text)
{
    cJSON *line = cJSON_Parse(text);
    if (!line)
        return;

    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(line, "kind"));
    if (kind) {
        for (size_t i = 0; i < sizeof LOADERS / sizeof *LOADERS; i++) {
            if (strcmp(kind, LOADERS[i].kind) != 0)
                continue;
            cJSON *st = cJSON_GetObjectItem(line, "state");
            if (st)
                LOADERS[i].load(st);
            cJSON_Delete(line);
            return;
        }
        load_rows(cJSON_GetObjectItem(line, "rows"));
    }
    cJSON_Delete(line);
}

int scrollback_restore(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    char  *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        if (line[n - 1] == '\n')
            line[n - 1] = '\0';
        load_line(line);
    }
    free(line);
    fclose(f);

    viewport_paint();
    return 1;
}
