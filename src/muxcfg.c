#include "muxcfg.h"

#include "muxmake.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "ask.h"
#include "cmd.h"
#include "pick.h"
#include "session.h"
#include "settings.h"
#include "text.h"
#include "ui.h"
#include "vendor/agents/backend.h"
#include "vendor/cJSON.h"

#define MUX_FILE    "matrix.json"
#define MUX_DEFAULT "claude,codex,grok"

struct set {
    char            name[MUX_NAME];
    int             n;
    struct mux_spec row[MUX_MAX];
};

static struct set sets[MUX_SETS];
static int        nsets;
static int        active;
static int        loaded;

static int name_taken(const char *name, int except);

static int known_backend(const char *name)
{
    for (const char *const *p = backend_names(); *p; p++)
        if (strcmp(name, *p) == 0)
            return 1;
    return 0;
}

static int store_path(char *out, size_t size)
{
    return path_config_file(out, size, MUX_FILE);
}

// The first format: one settings line of backend[:model[:effort]] entries, a
// bare name meaning the model last picked for that backend.
static void seed_from_settings(struct set *s)
{
    const char *p = settings_get_str(SETTING_MUX_BACKENDS, MUX_DEFAULT);

    while (*p && s->n < MUX_MAX) {
        while (*p == ',' || *p == ' ')
            p++;
        if (!*p)
            break;

        struct mux_spec m = {0};
        size_t          run = strcspn(p, ",");
        int             bare = run == strcspn(p, ":,");

        for (int f = 0; f < 3; f++) {
            char  *out = f == 0 ? m.backend : f == 1 ? m.model : m.effort;
            size_t cap = f == 0 ? sizeof m.backend
                       : f == 1 ? sizeof m.model
                                : sizeof m.effort;
            size_t len = strcspn(p, ":,");
            if (len >= cap)
                len = cap - 1;
            memcpy(out, p, len);
            out[len] = '\0';
            p += strcspn(p, ":,");
            if (*p != ':' || bare)
                break;
            p++;
        }
        p += strcspn(p, ",");

        if (!*m.backend || !known_backend(m.backend))
            continue;
        if (bare) {
            const char *model = session_saved_model(m.backend);
            const char *effort = session_saved_effort(m.backend);
            if (model)
                snprintf(m.model, sizeof m.model, "%s", model);
            if (effort)
                snprintf(m.effort, sizeof m.effort, "%s", effort);
        }
        s->row[s->n++] = m;
    }
}

static void put_field(char *out, size_t cap, const cJSON *obj, const char *key)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(obj, key));
    snprintf(out, cap, "%s", v ? v : "");
}

static void load_file(void)
{
    char path[4096];
    if (!store_path(path, sizeof path))
        return;

    char *text = text_slurp(path, 1 << 20, NULL);
    if (!text)
        return;

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root)
        return;

    const char *want = cJSON_GetStringValue(cJSON_GetObjectItem(root, "active"));
    cJSON      *configs = cJSON_GetObjectItem(root, "configs");
    cJSON      *config;

    cJSON_ArrayForEach(config, configs) {
        if (nsets >= MUX_SETS || !config->string)
            break;
        struct set *s = &sets[nsets++];
        snprintf(s->name, sizeof s->name, "%s", config->string);

        cJSON *row;
        cJSON_ArrayForEach(row, config) {
            if (s->n >= MUX_MAX)
                break;
            struct mux_spec m = {0};
            put_field(m.backend, sizeof m.backend, row, "backend");
            put_field(m.model, sizeof m.model, row, "model");
            put_field(m.effort, sizeof m.effort, row, "effort");
            put_field(m.prompt, sizeof m.prompt, row, "prompt");
            if (*m.backend && known_backend(m.backend))
                s->row[s->n++] = m;
        }
        if (want && !strcmp(want, s->name))
            active = nsets - 1;
    }
    cJSON_Delete(root);
}

static void save_file(void)
{
    char path[4096];
    if (!store_path(path, sizeof path))
        return;

    cJSON *root = cJSON_CreateObject();
    cJSON *configs = cJSON_AddObjectToObject(root, "configs");
    cJSON_AddStringToObject(root, "active", muxcfg_active());

    for (int i = 0; i < nsets; i++) {
        cJSON *rows = cJSON_AddArrayToObject(configs, sets[i].name);
        for (int r = 0; r < sets[i].n; r++) {
            const struct mux_spec *m = &sets[i].row[r];
            cJSON                 *row = cJSON_CreateObject();
            cJSON_AddStringToObject(row, "backend", m->backend);
            cJSON_AddStringToObject(row, "model", m->model);
            cJSON_AddStringToObject(row, "effort", m->effort);
            cJSON_AddStringToObject(row, "prompt", m->prompt);
            cJSON_AddItemToArray(rows, row);
        }
    }

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text)
        return;

    char temp[sizeof path + 8];
    if (snprintf(temp, sizeof temp, "%s.tmp", path) < (int)sizeof temp) {
        FILE *f = fopen(temp, "w");
        if (f) {
            int wrote = fputs(text, f) >= 0 && fputc('\n', f) != EOF;
            if (fclose(f) != 0 || !wrote || rename(temp, path) != 0)
                unlink(temp);
        }
    }
    free(text);
}

// The store, read once. An empty one starts from the older settings line.
static void ensure_loaded(void)
{
    if (loaded)
        return;
    loaded = 1;

    load_file();
    if (!nsets) {
        struct set *s = &sets[nsets++];
        snprintf(s->name, sizeof s->name, "default");
        seed_from_settings(s);
    }
    if (active >= nsets)
        active = 0;
}

const char *muxcfg_active(void)
{
    ensure_loaded();
    return sets[active].name;
}

int muxcfg_load(struct mux_spec *out, int max)
{
    ensure_loaded();

    int n = sets[active].n < max ? sets[active].n : max;
    memcpy(out, sets[active].row, (size_t)n * sizeof *out);
    return n;
}

// A name the store can tell apart from the ones already in it.
static void unique_name(const char *want, char *out, size_t cap)
{
    snprintf(out, cap, "%s", want && *want ? want : "matrix");
    for (int i = 2; name_taken(out, -1) && i < 100; i++)
        snprintf(out, cap, "%.*s %d", (int)cap - 5, want, i);
}

int muxcfg_install(const char *name, const struct mux_spec *v, int n)
{
    ensure_loaded();

    struct set *s;
    if (nsets < MUX_SETS) {
        s = &sets[nsets++];
    } else {
        // Full: the oldest one that is not in use makes way.
        s = &sets[active == 0 ? 1 : 0];
    }
    memset(s, 0, sizeof *s);

    char unique[MUX_NAME];
    unique_name(name, unique, sizeof unique);
    snprintf(s->name, sizeof s->name, "%s", unique);

    for (int i = 0; i < n && s->n < MUX_MAX; i++)
        if (*v[i].backend && known_backend(v[i].backend))
            s->row[s->n++] = v[i];

    active = (int)(s - sets);
    save_file();
    return s->n;
}

static const char *short_model(const struct mux_spec *m)
{
    if (!*m->model)
        return "default";
    if (!strcmp(m->backend, "claude") && !strncmp(m->model, "claude-", 7) && m->model[7])
        return m->model + 7;
    return m->model;
}

void muxcfg_label(const struct mux_spec *m, char *out, size_t cap)
{
    if (*m->effort && strcmp(m->effort, "default"))
        snprintf(out, cap, "%s \xc2\xb7 %s \xc2\xb7 %s", m->backend, short_model(m), m->effort);
    else
        snprintf(out, cap, "%s \xc2\xb7 %s", m->backend, short_model(m));
}

// "default" is how the pickers spell "leave it to the CLI", which is the empty
// string in the store.
static int pick_field(const char *title, const struct pick_item *items, int count,
                      char *out, size_t cap, int filter)
{
    int initial = 0;
    for (int i = 0; i < count; i++)
        if (!strcmp(items[i].label, *out ? out : "default"))
            initial = i;

    int index = filter ? pick_run_filter(title, items, count, initial)
                       : pick_run(title, items, count, initial);
    if (index < 0)
        return 0;
    snprintf(out, cap, "%s", strcmp(items[index].label, "default") ? items[index].label : "");
    return 1;
}

static int pick_model(struct mux_spec *m)
{
    int                     count = 0;
    const struct pick_item *items = cmd_model_choices(m->backend, &count);
    return pick_field("model", items, count, m->model, sizeof m->model, 1);
}

static int pick_effort(struct mux_spec *m)
{
    int                     count = 0;
    const struct pick_item *items = cmd_effort_choices(m->backend, &count);
    return pick_field("effort", items, count, m->effort, sizeof m->effort, 0);
}

static int pick_backend(char *out, size_t cap, const char *current)
{
    struct pick_item items[16];
    int              count = 0;

    for (const char *const *p = backend_names(); *p && count < (int)COUNT(items); p++)
        items[count++] = (struct pick_item){*p, NULL};
    if (!count)
        return 0;

    int initial = 0;
    for (int i = 0; i < count; i++)
        if (current && !strcmp(items[i].label, current))
            initial = i;

    int index = pick_run("backend", items, count, initial);
    if (index < 0)
        return 0;
    snprintf(out, cap, "%s", items[index].label);
    return 1;
}

// Standing instructions, put in front of whatever the row is asked.
static void row_prompt(struct mux_spec *m)
{
    char title[160];
    char label[80];
    muxcfg_label(m, label, sizeof label);
    snprintf(title, sizeof title, "%s \xc2\xb7 always tell it", label);

    char *text = ask_run(title, m->prompt);
    if (!text)
        return;
    snprintf(m->prompt, sizeof m->prompt, "%s", text);
    free(text);
}

// Models and efforts belong to a backend: the old ones cannot follow.
static void row_backend(struct mux_spec *m)
{
    char was[32];
    snprintf(was, sizeof was, "%s", m->backend);

    if (!pick_backend(m->backend, sizeof m->backend, was) || !strcmp(was, m->backend))
        return;
    m->model[0] = m->effort[0] = '\0';
    pick_model(m);
    pick_effort(m);
}

static int add_row(struct mux_spec *v, int n)
{
    if (n >= MUX_MAX)
        return n;

    struct mux_spec m = {0};
    if (!pick_backend(m.backend, sizeof m.backend, NULL))
        return n;
    if (!pick_model(&m))
        return n;
    pick_effort(&m);
    v[n++] = m;
    return n;
}

static int row_duplicate(struct mux_spec *v, int n, int at)
{
    if (n >= MUX_MAX)
        return n;
    memmove(&v[at + 2], &v[at + 1], (size_t)(n - at - 1) * sizeof *v);
    v[at + 1] = v[at];
    return n + 1;
}

static int row_remove(struct mux_spec *v, int n, int at)
{
    memmove(&v[at], &v[at + 1], (size_t)(n - at - 1) * sizeof *v);
    return n - 1;
}

static int edit_row(struct mux_spec *v, int n, int at)
{
    static const struct pick_item ACTIONS[] = {
        {"backend", "answer from another CLI"},
        {"model", "answer with another model"},
        {"effort", "how hard it thinks"},
        {"prompt", "standing instructions for this row"},
        {"duplicate", "another row on the same backend"},
        {"remove", "drop this row from the matrix"},
    };

    char title[160];
    muxcfg_label(&v[at], title, sizeof title);

    switch (pick_run(title, ACTIONS, (int)COUNT(ACTIONS), 0)) {
    case 0:
        row_backend(&v[at]);
        break;
    case 1:
        pick_model(&v[at]);
        break;
    case 2:
        pick_effort(&v[at]);
        break;
    case 3:
        row_prompt(&v[at]);
        break;
    case 4:
        n = row_duplicate(v, n, at);
        break;
    case 5:
        n = row_remove(v, n, at);
        break;
    }
    return n;
}

static int name_taken(const char *name, int except)
{
    for (int i = 0; i < nsets; i++)
        if (i != except && !strcmp(sets[i].name, name))
            return 1;
    return 0;
}

static int name_config(const char *title, char *out, size_t cap, int except)
{
    char *text = ask_run(title, except >= 0 ? sets[except].name : NULL);
    if (!text)
        return 0;

    text_chomp(text);
    if (!*text || name_taken(text, except)) {
        free(text);
        return 0;
    }
    snprintf(out, cap, "%s", text);
    free(text);
    return 1;
}

// The named matrices: one is in use, and the rest wait their turn.
static void configs_menu(void)
{
    for (;;) {
        char             labels[MUX_SETS][MUX_NAME + 8];
        char             details[MUX_SETS][32];
        struct pick_item items[MUX_SETS + 2];
        int              count = 0;

        for (int i = 0; i < nsets; i++) {
            snprintf(labels[i], sizeof labels[i], "%s%s",
                     i == active ? "\xe2\x97\x8f " : "  ", sets[i].name);
            snprintf(details[i], sizeof details[i], "%d row%s", sets[i].n,
                     sets[i].n == 1 ? "" : "s");
            items[count++] = (struct pick_item){labels[i], details[i]};
        }
        if (nsets < MUX_SETS) {
            items[count++] = (struct pick_item){"new config", "an empty matrix"};
            items[count++] = (struct pick_item){"describe one",
                                                "say what you want in it"};
        }
        items[count++] = (struct pick_item){"back", NULL};

        int pressed = 0;
        int index = pick_run_keys("configs \xc2\xb7 n new, s describe, r rename, "
                                  "x remove",
                                  items, count, active, "nsrx", &pressed);
        if (index < 0)
            break;

        if (pressed == 's' || (!pressed && index == nsets + 1 && nsets < MUX_SETS)) {
            char *what = ask_run("what should be in it", NULL);
            if (what) {
                muxmake_run(what);
                free(what);
            }
            continue;
        }
        if (pressed == 'n' || (!pressed && index == nsets && nsets < MUX_SETS)) {
            char name[MUX_NAME];
            if (nsets < MUX_SETS && name_config("name it", name, sizeof name, -1)) {
                snprintf(sets[nsets].name, sizeof sets[nsets].name, "%s", name);
                sets[nsets].n = 0;
                active = nsets++;
                save_file();
            }
            continue;
        }
        if (index >= nsets) {
            if (!pressed)
                break;
            continue;
        }

        switch (pressed) {
        case 'r':
            if (name_config("rename it", sets[index].name, sizeof sets[index].name, index))
                save_file();
            break;
        case 'x':
            if (nsets < 2)
                break;
            memmove(&sets[index], &sets[index + 1],
                    (size_t)(nsets - index - 1) * sizeof *sets);
            nsets--;
            if (active >= nsets)
                active = nsets - 1;
            else if (active > index)
                active--;
            save_file();
            break;
        default:
            active = index;
            save_file();
            return;
        }
    }
}

void muxcfg_run(void)
{
    ensure_loaded();

    struct set *s = &sets[active];
    int         sel = 0;

    for (;;) {
        char             labels[MUX_MAX][160];
        char             details[MUX_MAX][MUX_PROMPT];
        char             config[MUX_NAME + 16];
        struct pick_item items[MUX_MAX + 3];
        int              count = 0;

        s = &sets[active];
        for (int i = 0; i < s->n; i++) {
            muxcfg_label(&s->row[i], labels[i], sizeof labels[i]);
            snprintf(details[i], sizeof details[i], "%s", s->row[i].prompt);
            items[count++] = (struct pick_item){labels[i], details[i]};
        }
        if (s->n < MUX_MAX)
            items[count++] = (struct pick_item){"add a row", "another backend and model"};
        snprintf(config, sizeof config, "config: %s", s->name);
        items[count++] = (struct pick_item){config, "switch, rename, or start another"};
        items[count++] = (struct pick_item){"done", NULL};

        if (sel >= count)
            sel = count - 1;

        int pressed = 0;
        int index = pick_run_keys("mux matrix \xc2\xb7 a add, d duplicate, b backend, "
                                  "m model, e effort, p prompt, x remove, c configs",
                                  items, count, sel, "admbepxc", &pressed);
        if (index < 0)
            break;
        sel = index;

        // Adding and the config list need no row under the cursor.
        if (pressed == 'c') {
            configs_menu();
            sel = 0;
            continue;
        }
        if (pressed == 'a') {
            s->n = add_row(s->row, s->n);
            sel = s->n > 0 ? s->n - 1 : 0;
        } else if (index >= s->n) {
            if (pressed)
                continue;
            if (index == count - 1)
                break;
            if (index == count - 2) {
                configs_menu();
                sel = 0;
                continue;
            }
            s->n = add_row(s->row, s->n);
        } else {
            switch (pressed) {
            case 'd':
                s->n = row_duplicate(s->row, s->n, index);
                break;
            case 'b':
                row_backend(&s->row[index]);
                break;
            case 'm':
                pick_model(&s->row[index]);
                break;
            case 'e':
                pick_effort(&s->row[index]);
                break;
            case 'p':
                row_prompt(&s->row[index]);
                break;
            case 'x':
                s->n = row_remove(s->row, s->n, index);
                if (sel >= s->n)
                    sel = s->n > 0 ? s->n - 1 : 0;
                break;
            default:
                s->n = edit_row(s->row, s->n, index);
                break;
            }
        }
        save_file();
    }

    save_file();

    s = &sets[active];
    ui_note("mux matrix \xc2\xb7 %s", s->name);
    ui_put("\n");
    for (int i = 0; i < s->n; i++) {
        char label[160];
        muxcfg_label(&s->row[i], label, sizeof label);
        if (*s->row[i].prompt)
            ui_note("%s \xe2\x80\x94 %s", label, s->row[i].prompt);
        else
            ui_note("%s", label);
        ui_put("\n");
    }
    ui_flush();
}
