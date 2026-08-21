#include "models.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pick.h"
#include "text.h"
#include "vendor/cJSON.h"

#define LABEL_BYTES  96
#define DETAIL_BYTES 96

// A catalog costs a file read, or a CLI call over the network, so each backend
// is filled once and kept for the run.
struct list {
    char              backend[32];
    int               n, cap;
    struct pick_item *items;
    char            (*label)[LABEL_BYTES];
    char            (*detail)[DETAIL_BYTES];
};

static struct list cache[6];

static int grow(struct list *l)
{
    int cap = l->cap ? l->cap * 2 : 32;

    struct pick_item *items = realloc(l->items, (size_t)cap * sizeof *items);
    if (items)
        l->items = items;
    char (*label)[LABEL_BYTES] = realloc(l->label, (size_t)cap * sizeof *l->label);
    if (label)
        l->label = label;
    char (*detail)[DETAIL_BYTES] = realloc(l->detail, (size_t)cap * sizeof *l->detail);
    if (detail)
        l->detail = detail;
    if (!items || !label || !detail)
        return 0;

    l->cap = cap;
    for (int i = 0; i < l->n; i++)
        l->items[i] = (struct pick_item){l->label[i], l->detail[i]};
    return 1;
}

static void push(struct list *l, const char *label, const char *detail)
{
    if (!label || !*label)
        return;
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->label[i], label))
            return;
    if (l->n == l->cap && !grow(l))
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

// "Z.AI: GLM 5.2 · 200K context"
static void describe(char *out, size_t cap, const char *name, double context)
{
    char ctx[32] = "";
    if (context >= 1000000)
        snprintf(ctx, sizeof ctx, "%gM context", (double)(long)(context / 100000) / 10);
    else if (context >= 1000)
        snprintf(ctx, sizeof ctx, "%ldK context", (long)(context / 1000));

    if (name && *name && ctx[0])
        snprintf(out, cap, "%s \xc2\xb7 %s", name, ctx);
    else
        snprintf(out, cap, "%s", name && *name ? name : ctx);
}

// OpenRouter ids are namespaced by vendor ("z-ai/glm-5.2"); pi wants the
// provider in front, except for its own routers, which already read that way.
static void pi_model_id(char *out, size_t cap, const char *id)
{
    if (!strncmp(id, "openrouter/", 11))
        snprintf(out, cap, "%s", id);
    else
        snprintf(out, cap, "openrouter/%s", id);
}

static void push_openrouter(struct list *l, const char *id, const char *name, double context)
{
    if (!id || !*id)
        return;

    char label[LABEL_BYTES];
    char detail[DETAIL_BYTES];
    pi_model_id(label, sizeof label, id);
    describe(detail, sizeof detail, name, context);
    push(l, label, detail);

    // The routers take OpenRouter's routing suffixes; on a plain model they are
    // a shortcut worth typing by hand, not 400 more rows.
    if (strncmp(id, "openrouter/", 11))
        return;
    static const char *const VARIANTS[] = {":nitro", ":floor"};
    static const char *const WHAT[] = {"routed for throughput", "routed for price"};
    for (size_t i = 0; i < sizeof VARIANTS / sizeof *VARIANTS; i++) {
        char variant[LABEL_BYTES];
        char why[DETAIL_BYTES];
        snprintf(variant, sizeof variant, "%s%s", label, VARIANTS[i]);
        snprintf(why, sizeof why, "%s \xc2\xb7 %s", name && *name ? name : id, WHAT[i]);
        push(l, variant, why);
    }
}

static void openrouter_json(struct list *l, const char *text, const char *key)
{
    cJSON *root = cJSON_Parse(text);
    cJSON *models = root ? cJSON_GetObjectItem(root, key) : NULL;

    // Routers first: they are the ones worth scrolling to rather than typing.
    for (int routers = 1; routers >= 0; routers--) {
        cJSON *m;
        cJSON_ArrayForEach(m, models) {
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(m, "id"));
            if (!id || (strncmp(id, "openrouter/", 11) == 0) != routers)
                continue;
            push_openrouter(l, id, cJSON_GetStringValue(cJSON_GetObjectItem(m, "name")),
                            cJSON_GetNumberValue(cJSON_GetObjectItem(m, "context_length")));
        }
    }
    cJSON_Delete(root);
}

// Four hours, the same window pi revalidates its own catalogs on.
#define CATALOG_MAX_AGE (4 * 60 * 60)

// Refresh out of band, the way pi does: whatever is on disk is what this run
// shows, and the fetch lands for the next time the picker opens. openrouter.ai
// serves the catalog with no etag and no last-modified, so a conditional
// request buys nothing and every refresh is a full body. A fetch that fails
// leaves the mtime alone, so the next picker retries instead of sitting on a
// stale catalog for the rest of the window.
static void refresh_openrouter(const char *path)
{
    char dest[4200], tmp[4300], quoted[4400];
    if (snprintf(tmp, sizeof tmp, "%s.new", path) >= (int)sizeof tmp)
        return;
    if (!text_shell_quote(path, dest, sizeof dest) ||
        !text_shell_quote(tmp, quoted, sizeof quoted))
        return;

    char cmd[26000];
    if (snprintf(cmd, sizeof cmd,
                 "(if curl -fsS --max-time 20 -o %s "
                 "https://openrouter.ai/api/v1/models >/dev/null 2>&1 && [ -s %s ]; "
                 "then mv %s %s; else rm -f %s; fi) >/dev/null 2>&1 &",
                 quoted, quoted, quoted, dest, quoted) >= (int)sizeof cmd)
        return;

    (void)system(cmd);
}

// The full OpenRouter catalog, kept in ~/.config/mux. Routers like
// openrouter/pareto-code are only in openrouter.ai's own list, not the mirror
// pi caches, so this reads from the source.
static int fill_openrouter_catalog(struct list *l)
{
    char path[4096];
    if (!path_config_file(path, sizeof path, "openrouter.json"))
        return 0;

    struct stat st;
    if (stat(path, &st) || st.st_size < 1024 ||
        now_seconds() - (double)st.st_mtime > CATALOG_MAX_AGE)
        refresh_openrouter(path);

    char *text = text_slurp(path, 1 << 23, NULL);
    if (!text)
        return 0;

    int before = l->n;
    openrouter_json(l, text, "data");
    free(text);
    return l->n > before;
}

// ~/.pi/agent/models-store.json: the catalogs pi itself has cached.
static void fill_pi_store(struct list *l)
{
    char *text = home_slurp(".pi/agent/models-store.json");
    if (!text)
        return;

    cJSON *root = cJSON_Parse(text);
    cJSON *openrouter = root ? cJSON_GetObjectItem(root, "openrouter") : NULL;
    cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(openrouter, "models"))
        push_openrouter(l, cJSON_GetStringValue(cJSON_GetObjectItem(m, "id")),
                        cJSON_GetStringValue(cJSON_GetObjectItem(m, "name")),
                        cJSON_GetNumberValue(cJSON_GetObjectItem(m, "contextWindow")));
    cJSON_Delete(root);
    free(text);
}

// ~/.pi/agent/models.json: the models pi has been configured with by hand.
// They go first, since they are the ones already chosen.
static void fill_pi_configured(struct list *l)
{
    char *text = home_slurp(".pi/agent/models.json");
    if (!text)
        return;

    cJSON *root = cJSON_Parse(text);
    cJSON *providers = root ? cJSON_GetObjectItem(root, "providers") : NULL;
    cJSON *provider;
    cJSON_ArrayForEach(provider, providers) {
        cJSON *m;
        cJSON_ArrayForEach(m, cJSON_GetObjectItem(provider, "models")) {
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(m, "id"));
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(m, "name"));
            char detail[DETAIL_BYTES];
            describe(detail, sizeof detail, name,
                     cJSON_GetNumberValue(cJSON_GetObjectItem(m, "contextWindow")));
            if (detail[0])
                snprintf(detail + strlen(detail), sizeof detail - strlen(detail),
                         " \xc2\xb7 %s", provider->string);
            else
                snprintf(detail, sizeof detail, "%s", provider->string);
            push(l, id, detail);
        }
    }
    cJSON_Delete(root);
    free(text);
}

static void fill_pi(struct list *l)
{
    fill_pi_configured(l);
    if (!fill_openrouter_catalog(l))
        fill_pi_store(l);
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

    char detail[DETAIL_BYTES];
    snprintf(detail, sizeof detail, "whatever the %s CLI is configured to use", backend);
    push(l, "default", detail);

    if (!strcmp(backend, "claude")) {
        fill_static(l, CLAUDE, (int)(sizeof CLAUDE / sizeof *CLAUDE));
    } else if (!strcmp(backend, "codex")) {
        fill_codex(l);
    } else if (!strcmp(backend, "pi")) {
        fill_pi(l);
    } else if (!strcmp(backend, "grok")) {
        int before = l->n;
        fill_grok(l);
        if (l->n == before)
            fill_static(l, GROK, (int)(sizeof GROK / sizeof *GROK));
    }

    *out = l->items;
    return l->n;
}
