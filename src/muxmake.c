#include "muxmake.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "cmd.h"
#include "models.h"
#include "muxcfg.h"
#include "pick.h"
#include "restart.h"
#include "session.h"
#include "status.h"
#include "ui.h"
#include "vendor/agents/backend.h"
#include "vendor/cJSON.h"

#define MAKE_SPEC       8192
#define SPEC_MAX_MODELS 40

struct work {
    const char *prompt;
    const char *system;
    char       *reply;
    char        error[256];
    atomic_int  done;
};

static atomic_int aborted;

static int make_aborted(void) { return atomic_load(&aborted); }

__attribute__((format(printf, 4, 5)))
static void spec_add(char *out, size_t cap, size_t *at, const char *fmt, ...)
{
    if (*at + 1 >= cap)
        return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *at, cap - *at, fmt, ap);
    va_end(ap);

    if (n < 0)
        return;
    *at += (size_t)n < cap - *at ? (size_t)n : cap - *at - 1;
}

// What the model is allowed to pick from: the installed CLIs, and the models
// and efforts each of them offers here.
static void backend_spec(char *out, size_t cap)
{
    size_t at = 0;

    spec_add(out, cap, &at, "The backends, and what each accepts:\n");

    for (const char *const *b = backend_names(); *b; b++) {
        const struct pick_item *models = NULL;
        const struct pick_item *efforts = NULL;
        int                     nmodels = models_for(*b, &models);
        int                     nefforts = 0;
        int                     shown = nmodels < SPEC_MAX_MODELS ? nmodels : SPEC_MAX_MODELS;

        efforts = cmd_effort_choices(*b, &nefforts);

        spec_add(out, cap, &at, "\n%s\n  models:", *b);
        for (int i = 0; i < shown; i++)
            spec_add(out, cap, &at, " %s", models[i].label);
        if (shown < nmodels)
            spec_add(out, cap, &at, " ... and %d more", nmodels - shown);
        spec_add(out, cap, &at, "\n  efforts:");
        for (int i = 0; i < nefforts; i++)
            spec_add(out, cap, &at, " %s", efforts[i].label);
        spec_add(out, cap, &at, "\n");
    }
}

static void make_system(char *out, size_t cap)
{
    char spec[MAKE_SPEC];
    backend_spec(spec, sizeof spec);

    snprintf(out, cap,
             "You lay out a mux matrix: a set of agents asked the same question "
             "at once, each answering in its own voice.\n\n"
             "%s\n"
             "Answer with JSON and nothing else:\n"
             "{\"name\": \"two or three words\", \"rows\": [{\"backend\": \"...\", "
             "\"model\": \"...\", \"effort\": \"...\", \"prompt\": \"...\"}]}\n\n"
             "One row per agent, at most %d. `model` and `effort` must come from "
             "the lists above, or be \"\" for that CLI's own default. `prompt` is "
             "the standing instruction that gives the row its character, written "
             "as an instruction to that agent; leave it \"\" when the row is "
             "meant to answer plainly. Where the description asks for several of "
             "the same thing, repeat the backend as separate rows. Prefer the "
             "backend the description names; use claude when it names none.",
             spec, MUX_MAX);
}

static void *make_work(void *ud)
{
    struct work *w = ud;

    restart_shield_thread();

    backend_opts o = {0};
    o.name = "claude";
    o.system = w->system;
    o.session_name = APP_NAME " matrix";
    o.ephemeral = 1;
    o.disable_tools = 1;

    Backend *b = backend_open_ex(&o);
    if (!b) {
        snprintf(w->error, sizeof w->error, "could not open claude");
        atomic_store(&w->done, 1);
        return NULL;
    }
    b->set_abort_check(b, make_aborted);

    w->reply = b->ask(b, w->prompt);
    if (!w->reply) {
        const char *why = b->last_error ? b->last_error(b) : NULL;
        snprintf(w->error, sizeof w->error, "%s", why && *why ? why : "no reply");
    }
    b->close(b);
    atomic_store(&w->done, 1);
    return NULL;
}

// Models fence their JSON as often as not.
static const char *json_start(const char *reply)
{
    const char *brace = strchr(reply, '{');
    return brace ? brace : reply;
}

static void field(char *out, size_t cap, const cJSON *row, const char *key)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(row, key));
    snprintf(out, cap, "%s", v ? v : "");
}

// An effort the CLI does not know is worse than none: it fails the whole turn.
static void keep_known_effort(struct mux_spec *m)
{
    if (!*m->effort)
        return;

    int                     count = 0;
    const struct pick_item *items = cmd_effort_choices(m->backend, &count);
    for (int i = 0; i < count; i++)
        if (!strcmp(items[i].label, m->effort)) {
            if (!strcmp(m->effort, "default"))
                m->effort[0] = '\0';
            return;
        }
    m->effort[0] = '\0';
}

static int install_reply(const char *reply)
{
    cJSON *root = cJSON_Parse(json_start(reply));
    if (!root) {
        ui_error("that did not come back as a matrix");
        ui_put("\n");
        ui_flush();
        return 0;
    }

    struct mux_spec v[MUX_MAX];
    int             n = 0;

    cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItem(root, "rows")) {
        if (n >= MUX_MAX)
            break;
        struct mux_spec m = {0};
        field(m.backend, sizeof m.backend, row, "backend");
        field(m.model, sizeof m.model, row, "model");
        field(m.effort, sizeof m.effort, row, "effort");
        field(m.prompt, sizeof m.prompt, row, "prompt");
        if (!strcmp(m.model, "default"))
            m.model[0] = '\0';
        keep_known_effort(&m);
        if (*m.backend)
            v[n++] = m;
    }

    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    int         kept = n ? muxcfg_install(name, v, n) : 0;
    cJSON_Delete(root);

    if (!kept) {
        ui_error("nothing usable came back");
        ui_put("\n");
        ui_flush();
    }
    return kept;
}

int muxmake_run(const char *description)
{
    if (!description || !*description)
        return 0;

    char system[MAKE_SPEC + 1024];
    make_system(system, sizeof system);

    struct work w = {.prompt = description, .system = system};
    pthread_t   thread;

    atomic_store(&aborted, 0);
    if (pthread_create(&thread, NULL, make_work, &w) != 0) {
        ui_error("could not start a worker");
        ui_put("\n");
        ui_flush();
        return 0;
    }

    status_set_word("laying out the matrix");
    status_begin();
    while (!atomic_load(&w.done)) {
        if (session_poll_input())
            atomic_store(&aborted, 1);
        status_tick();

        struct timespec nap = {0, 15 * 1000 * 1000};
        nanosleep(&nap, NULL);
    }
    status_end();
    pthread_join(thread, NULL);

    int kept = 0;
    if (w.reply) {
        kept = install_reply(w.reply);
    } else {
        ui_error("%s", w.error);
        ui_put("\n");
        ui_flush();
    }
    free(w.reply);
    return kept;
}
