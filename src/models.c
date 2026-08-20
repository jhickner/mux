#include "models.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pick.h"
#include "text.h"
#include "vendor/cJSON.h"

#define MODELS_MAX   24
#define LABEL_BYTES  64
#define DETAIL_BYTES 96

struct list {
    char             backend[32];
    int              n;
    struct pick_item items[MODELS_MAX];
    char             label[MODELS_MAX][LABEL_BYTES];
    char             detail[MODELS_MAX][DETAIL_BYTES];
};

// Read once and kept: a catalog costs a file, or a CLI call over the network.
static struct list cache[6];

static void push(struct list *l, const char *label, const char *detail)
{
    if (l->n >= MODELS_MAX || !label || !*label)
        return;
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->label[i], label))
            return;

    int at = l->n++;
    snprintf(l->label[at], LABEL_BYTES, "%s", label);
    snprintf(l->detail[at], DETAIL_BYTES, "%s", detail ? detail : "");
    l->items[at] = (struct pick_item){l->label[at], l->detail[at]};
}

static char *home_slurp(const char *rest)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        return NULL;

    char path[4096];
    snprintf(path, sizeof path, "%s/%s", home, rest);
    return text_slurp(path, 1 << 22, NULL);
}

static const struct pick_item CLAUDE[] = {
    {"claude-opus-5", "most capable"},
    {"claude-opus-5[1m]", "opus with a 1M-token context"},
    {"claude-sonnet-5", "balanced speed and capability"},
    {"claude-haiku-4-5", "fastest"},
    {"claude-fable-5", "compact"},
};

static const struct pick_item GROK[] = {
    {"grok-4.6", "latest frontier model"},
    {"grok-4.5", "the prior generation"},
};

static void fill_static(struct list *l, const struct pick_item *v, int n)
{
    for (int i = 0; i < n; i++)
        push(l, v[i].label, v[i].detail);
}

// ~/.codex/models_cache.json: what the Codex picker itself offers.
static void fill_codex(struct list *l)
{
    char *text = home_slurp(".codex/models_cache.json");
    if (!text)
        return;

    cJSON *root = cJSON_Parse(text);
    cJSON *models = root ? cJSON_GetObjectItem(root, "models") : NULL;
    cJSON *m;
    cJSON_ArrayForEach(m, models) {
        const char *visibility = cJSON_GetStringValue(cJSON_GetObjectItem(m, "visibility"));
        if (visibility && strcmp(visibility, "list"))
            continue;
        push(l, cJSON_GetStringValue(cJSON_GetObjectItem(m, "slug")),
             cJSON_GetStringValue(cJSON_GetObjectItem(m, "description")));
    }
    cJSON_Delete(root);
    free(text);
}

// ~/.pi/agent/models.json: the catalogs pi has been pointed at, by provider.
static void fill_pi(struct list *l)
{
    char *text = home_slurp(".pi/agent/models.json");
    if (!text)
        return;

    cJSON *root = cJSON_Parse(text);
    cJSON *providers = root ? cJSON_GetObjectItem(root, "providers") : NULL;
    cJSON *provider;
    cJSON_ArrayForEach(provider, providers) {
        cJSON *m;
        cJSON_ArrayForEach(m, cJSON_GetObjectItem(provider, "models"))
            push(l, cJSON_GetStringValue(cJSON_GetObjectItem(m, "id")), provider->string);
    }
    cJSON_Delete(root);
    free(text);
}

// grok lists its own: "  * grok-4.6 (default)" or "  - grok-4.5".
static void fill_grok(struct list *l)
{
    FILE *f = popen("grok models 2>/dev/null", "r");
    if (!f)
        return;

    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line + strspn(line, " \t");
        if (*p != '*' && *p != '-')
            continue;
        p += strspn(p + 1, " \t") + 1;

        size_t n = strcspn(p, " \t\r\n");
        if (!n || n >= LABEL_BYTES)
            continue;
        int fallback = strstr(p + n, "default") != NULL;
        p[n] = '\0';
        push(l, p, fallback ? "the CLI default" : NULL);
    }
    pclose(f);
}

int models_for(const char *backend, const struct pick_item **out)
{
    struct list *l = NULL;
    for (size_t i = 0; i < sizeof cache / sizeof *cache; i++) {
        if (!strcmp(cache[i].backend, backend)) {
            *out = cache[i].items;
            return cache[i].n;
        }
        if (!l && !cache[i].backend[0])
            l = &cache[i];
    }
    if (!l) {
        *out = NULL;
        return 0;
    }
    snprintf(l->backend, sizeof l->backend, "%s", backend);

    if (!strcmp(backend, "claude")) {
        fill_static(l, CLAUDE, (int)(sizeof CLAUDE / sizeof *CLAUDE));
    } else if (!strcmp(backend, "codex")) {
        fill_codex(l);
    } else if (!strcmp(backend, "pi")) {
        fill_pi(l);
    } else if (!strcmp(backend, "grok")) {
        fill_grok(l);
        if (!l->n)
            fill_static(l, GROK, (int)(sizeof GROK / sizeof *GROK));
    }

    char detail[DETAIL_BYTES];
    snprintf(detail, sizeof detail, "whatever the %s CLI is configured to use", backend);
    push(l, "default", detail);

    *out = l->items;
    return l->n;
}
