#include "fanout.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "filediff.h"
#include "md.h"
#include "restart.h"
#include "session.h"
#include "sessionview.h"
#include "settings.h"
#include "status.h"
#include "text.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"
#include "vendor/agents/backend.h"

#define FAN_MAX     8
#define FAN_ROSTER  "claude,codex,grok"

#define FAN_COL_MIN 14

enum fan_state { FAN_WORK, FAN_OK, FAN_FAIL };

enum fan_kind { FAN_SAID, FAN_THOUGHT, FAN_TOOL, FAN_RESULT, FAN_NOTE };

struct entry {
    enum fan_kind kind;
    long          seq;
    int           gap;
    int           failed;
    char         *text;
    char         *name;
    char         *arg;
    char         *diff;

    char         *painted;
    int           painted_width;
};

struct cell;

struct worker {
    char name[32];
    char model[128];
    char effort[32];
    char resolved[128];

    const char *cwd;
    const char *prompt;
    const char *permission;

    char       *reply;
    char        error[256];
    double      secs;
    int         interrupted;
    int         started;
    atomic_int  done;
    pthread_t   thread;

    int           state;

    struct entry *log;
    int           count;
    int           cap;
    int           after_activity;
    int           after_tool;

    struct cell  *rows;
    int           nrows;
    int           rows_cap;
    int           rows_width;
    int           laid;
};

// One transcript entry, redrawn in place while the turns run.
struct board {
    struct worker w[FAN_MAX];
    int           n;
    int           live;         /* still running: only the tail of a column shows */
};

static atomic_int      aborted;
static atomic_int      show_thinking;
static pthread_mutex_t board_lock = PTHREAD_MUTEX_INITIALIZER;

static int fan_aborted(void) { return atomic_load(&aborted); }

static int known_backend(const char *name)
{
    for (const char *const *p = backend_names(); *p; p++)
        if (strcmp(name, *p) == 0)
            return 1;
    return 0;
}

static struct entry *log_add(struct worker *w, enum fan_kind kind, const char *text)
{
    if (w->count == w->cap) {
        int cap = w->cap ? w->cap * 2 : 16;
        struct entry *grown = realloc(w->log, (size_t)cap * sizeof *grown);
        if (!grown)
            return NULL;
        w->log = grown;
        w->cap = cap;
    }
    static long seq;
    struct entry *e = &w->log[w->count];
    *e = (struct entry){.kind = kind, .seq = seq++};
    if (text && !(e->text = strdup(text)))
        return NULL;
    w->count++;
    return e;
}

static void log_free(struct worker *w)
{
    for (int i = 0; i < w->count; i++) {
        free(w->log[i].text);
        free(w->log[i].name);
        free(w->log[i].arg);
        free(w->log[i].diff);
        free(w->log[i].painted);
    }
    free(w->log);
    w->log = NULL;
    w->count = w->cap = w->laid = w->nrows = 0;
}

static void fan_event(void *ud, const backend_event *ev)
{
    struct worker *w = ud;

    pthread_mutex_lock(&board_lock);
    switch (ev->kind) {

    case BACKEND_EV_CWD:
    case BACKEND_EV_TRUST:
    case BACKEND_EV_WARNING:
        break;

    case BACKEND_EV_INIT:
        if (ev->name && *ev->name)
            snprintf(w->resolved, sizeof w->resolved, "%s", ev->name);
        break;

    case BACKEND_EV_ASSISTANT: {
        if (!ev->text || !*ev->text)
            break;
        struct entry *e = log_add(w, FAN_SAID, ev->text);
        if (e)
            e->gap = w->after_activity;
        w->after_activity = 0;
        w->after_tool = 0;
        break;
    }

    case BACKEND_EV_THINKING: {
        if (!atomic_load(&show_thinking) || !ev->text || !*ev->text)
            break;
        struct entry *e = log_add(w, FAN_THOUGHT, ev->text);
        if (e)
            e->gap = w->after_tool;
        w->after_activity = 1;
        w->after_tool = 1;
        break;
    }

    case BACKEND_EV_TOOL: {
        char arg[4096];
        view_tool_argument(ev, w->cwd, arg, sizeof arg);
        struct entry *e = log_add(w, FAN_TOOL, NULL);
        if (e) {
            e->gap = w->after_tool;
            e->name = strdup(ev->name ? ev->name : "?");
            e->arg = strdup(arg);
        }
        w->after_activity = 1;
        w->after_tool = 1;
        break;
    }

    case BACKEND_EV_TOOL_RESULT: {
        struct entry *e = log_add(w, FAN_RESULT, ev->text);
        if (e) {
            e->failed = ev->failed;
            if (ev->diff)
                e->diff = strdup(ev->diff);
        }
        w->after_activity = 1;
        w->after_tool = 1;
        break;
    }
    }
    pthread_mutex_unlock(&board_lock);
}

static const char *entry_painted(struct entry *e, int width)
{
    if (e->painted && e->painted_width == width)
        return e->painted;

    free(e->painted);
    ui_capture_begin(width);
    if (e->gap)
        ui_put("\n");
    switch (e->kind) {
    case FAN_SAID:
        md_render(e->text, 0);
        ui_put("\n");
        break;
    case FAN_THOUGHT:
        view_activity("\xe2\x9c\xbb", e->text, UI_THINKING);
        break;
    case FAN_TOOL:
        view_tool_call(e->name, e->arg);
        break;
    case FAN_RESULT:
        if (e->failed) {
            const char *why = e->text && *e->text ? e->text : NULL;
            if (!why || !strcmp(why, "failed")) {
                view_tool_output("failed", UI_ERROR);
            } else {
                char line[4096];
                snprintf(line, sizeof line, "failed: %s", why);
                view_tool_error(line);
            }
        } else if (!e->diff || !filediff_render_patch(e->diff)) {
            view_tool_output(e->text, UI_DIM);
        }
        break;
    case FAN_NOTE:
        ui_wrapped(e->text, 0, UI_ERROR);
        break;
    }
    e->painted = ui_capture_end();
    e->painted_width = width;
    return e->painted;
}

struct cell {
    const char *text;
    size_t      bytes;
    int         width;
};

static int board_width(const struct board *b, int cols)
{
    return (cols - 1) / b->n;
}

static void column_row(struct worker *w, const char *text, size_t bytes, int width)
{
    if (ui_cells_visible(text, bytes) > (size_t)width)
        bytes = ui_fit_visible(text, bytes, (size_t)width);

    if (w->nrows == w->rows_cap) {
        int cap = w->rows_cap ? w->rows_cap * 2 : 64;
        struct cell *grown = realloc(w->rows, (size_t)cap * sizeof *grown);
        if (!grown)
            return;
        w->rows = grown;
        w->rows_cap = cap;
    }
    w->rows[w->nrows++] = (struct cell){text, bytes, (int)ui_cells_visible(text, bytes)};
}

// Called with the board locked: the workers are still writing to their logs.
static void column_lay(struct worker *w, int width)
{
    if (w->rows_width != width) {
        w->rows_width = width;
        w->nrows = 0;
        w->laid = 0;
    }

    int count = w->count;

    for (; w->laid < count; w->laid++) {
        const char *p = entry_painted(&w->log[w->laid], width);
        while (p && *p) {
            const char *nl = strchr(p, '\n');
            column_row(w, p, nl ? (size_t)(nl - p) : strlen(p), width);
            if (!nl)
                break;
            p = nl + 1;
        }
    }
}

static void board_lay(struct board *b, int width)
{
    for (int i = 0; i < b->n; i++)
        column_lay(&b->w[i], width - 1);
}

static int board_height(const struct board *b)
{
    int tallest = 0;
    for (int i = 0; i < b->n; i++)
        if (b->w[i].nrows > tallest)
            tallest = b->w[i].nrows;
    return tallest;
}

static void board_row(const struct board *b, int width, const int *from, int r)
{
    int last = -1;
    for (int i = 0; i < b->n; i++)
        if (from[i] + r < b->w[i].nrows)
            last = i;

    for (int i = 0; i < b->n; i++) {
        if (i > last)
            break;
        int cell = width - (i + 1 < b->n ? 0 : 1);
        int at = from[i] + r;
        if (at >= b->w[i].nrows) {
            if (i < last)
                ui_pad(cell);
            continue;
        }
        const struct cell *c = &b->w[i].rows[at];
        ui_putn(c->text, c->bytes);

        ui_esc(ui_style(UI_RESET));
        if (i < last)
            ui_pad(cell - c->width);
    }
    ui_put("\n");
}

static void label_row(const struct board *b, int width, int closing)
{
    for (int i = 0; i < b->n; i++) {
        const struct worker *w = &b->w[i];
        char text[160];
        enum ui_role role = UI_BOLD;

        if (!closing) {
            snprintf(text, sizeof text, "%s", w->name);
        } else if (w->state == FAN_FAIL) {
            snprintf(text, sizeof text, "%s \xc2\xb7 failed", w->name);
            role = UI_ERROR;
        } else {
            snprintf(text, sizeof text, "%s \xc2\xb7 %.1fs", w->name, w->secs);
            role = UI_DIM;
        }

        size_t bytes = ui_fit_bytes(text, (size_t)(width - 1));
        ui_esc(ui_style(role));
        ui_putn(text, bytes);
        ui_esc(ui_style(UI_RESET));
        if (i + 1 < b->n)
            ui_pad(width - (int)ui_cells_n(text, bytes));
    }
    ui_put("\n");
}

// Too narrow for columns: one after another, each under its own heading.
static void board_stacked(struct board *b, int cols)
{
    for (int i = 0; i < b->n; i++) {
        struct worker *w = &b->w[i];
        ui_bar(ui_style(UI_CHROME), "%s \xc2\xb7 %.1fs", w->name, w->secs);
        ui_put("\n");
        for (int e = 0; e < w->count; e++) {
            const char *painted = entry_painted(&w->log[e], cols);
            if (painted)
                ui_put(painted);
        }
        ui_put("\n");
    }
}

// A cap, so a long turn does not push the rest of the transcript off screen.
static int live_rows(void)
{
    int rows = tty_rows() * 2 / 3;
    return rows < 6 ? 6 : rows;
}

static void board_render(void *ud, int cols)
{
    struct board *b = ud;

    // The workers are still writing to their logs.
    pthread_mutex_lock(&board_lock);

    int width = board_width(b, cols);
    if (width < FAN_COL_MIN) {
        board_stacked(b, cols);
        pthread_mutex_unlock(&board_lock);
        return;
    }

    board_lay(b, width);
    int height = board_height(b);
    int body = height;
    if (b->live && body > live_rows())
        body = live_rows();

    int from[FAN_MAX];
    for (int i = 0; i < b->n; i++) {
        int tail = b->w[i].nrows > body ? b->w[i].nrows - body : 0;
        from[i] = tail;
    }

    ui_put("\n");
    label_row(b, width, 0);
    for (int r = 0; r < body; r++)
        board_row(b, width, from, r);
    if (!b->live)
        label_row(b, width, 1);
    ui_put("\n");

    pthread_mutex_unlock(&board_lock);
}

static void board_free(void *ud)
{
    struct board *b = ud;
    for (int i = 0; i < b->n; i++) {
        free(b->w[i].reply);
        free(b->w[i].rows);
        log_free(&b->w[i]);
    }
    free(b);
}

static void *fan_work(void *arg)
{
    struct worker *w = arg;

    restart_shield_thread();

    backend_opts o = {0};
    o.name = w->name;
    o.model = *w->model ? w->model : NULL;
    o.effort = *w->effort ? w->effort : NULL;
    o.cwd = w->cwd;
    o.session_name = APP_NAME " fanout";
    o.ephemeral = 1;
    o.permission_mode = w->permission;

    Backend *b = backend_open_ex(&o);
    if (!b) {
        pthread_mutex_lock(&board_lock);
        w->state = FAN_FAIL;
        snprintf(w->error, sizeof w->error, "could not open %s", w->name);
        log_add(w, FAN_NOTE, w->error);
        pthread_mutex_unlock(&board_lock);
        atomic_store(&w->done, 1);
        return NULL;
    }
    b->set_abort_check(b, fan_aborted);
    b->set_event_cb(b, fan_event, w);

    double started = now_seconds();

    backend_result meta = {0};
    char *reply = b->ask_ex(b, w->prompt, &meta);

    if (b->model) {
        const char *got = b->model(b);
        if (got && *got) {
            pthread_mutex_lock(&board_lock);
            snprintf(w->resolved, sizeof w->resolved, "%s", got);
            pthread_mutex_unlock(&board_lock);
        }
    }
    const char *detail = reply || !b->last_error ? NULL : b->last_error(b);

    pthread_mutex_lock(&board_lock);
    w->secs = now_seconds() - started;
    w->interrupted = meta.interrupted;
    w->reply = reply;
    w->state = reply ? FAN_OK : FAN_FAIL;
    if (!reply) {
        snprintf(w->error, sizeof w->error, "%s",
                 detail && *detail ? detail : "no reply");

        log_add(w, FAN_NOTE, w->error);
    }
    if (meta.interrupted)
        log_add(w, FAN_NOTE, "interrupted");
    pthread_mutex_unlock(&board_lock);

    b->close(b);
    atomic_store(&w->done, 1);
    return NULL;
}

static int roster(struct worker *w, int max, const char *spec)
{
    int n = 0;
    const char *p = spec;
    while (*p && n < max) {
        while (*p == ',' || *p == ' ')
            p++;
        size_t len = strcspn(p, ", ");
        if (!len)
            break;
        char name[32];
        if (len < sizeof name) {
            memcpy(name, p, len);
            name[len] = '\0';
            int seen = 0;
            for (int i = 0; i < n; i++)
                seen |= strcmp(w[i].name, name) == 0;
            if (!seen && known_backend(name))
                snprintf(w[n++].name, sizeof w->name, "%s", name);
        }
        p += len;
    }
    return n;
}

int fanout_run(struct session *s, const char *prompt)
{
    if (!prompt || !*prompt)
        return 0;

    // The board outlives this call: the transcript keeps it.
    struct board *b = calloc(1, sizeof *b);
    if (!b)
        return 0;

    const char *spec = settings_get_str(SETTING_MUX_BACKENDS, FAN_ROSTER);
    int n = roster(b->w, FAN_MAX, spec);
    if (n < 1) {
        free(b);
        ui_error("no known backends in " SETTING_MUX_BACKENDS " (%s)", spec);
        ui_put("\n");
        ui_flush();
        return 0;
    }
    b->n = n;
    b->live = 1;

    for (int i = 0; i < n; i++) {
        struct worker *w = &b->w[i];
        const char *model = session_saved_model(w->name);
        const char *effort = session_saved_effort(w->name);
        if (model)
            snprintf(w->model, sizeof w->model, "%s", model);
        if (effort)
            snprintf(w->effort, sizeof w->effort, "%s", effort);
        w->cwd = session_cwd(s);
        w->prompt = prompt;
        w->permission = session_permission(s);
    }

    atomic_store(&aborted, 0);
    atomic_store(&show_thinking, session_thinking(s));
    for (int i = 0; i < n; i++) {
        struct worker *w = &b->w[i];
        if (pthread_create(&w->thread, NULL, fan_work, w) == 0) {
            w->started = 1;
        } else {
            pthread_mutex_lock(&board_lock);
            w->state = FAN_FAIL;
            snprintf(w->error, sizeof w->error, "could not start a worker");
            log_add(w, FAN_NOTE, w->error);
            pthread_mutex_unlock(&board_lock);
            atomic_store(&w->done, 1);
        }
    }

    // With no viewport, print it once at the end and free it here.
    unsigned mark = 0;
    int      kept = viewport_active();
    if (kept) {
        mark = viewport_item_begin(board_render, b, board_free);
        board_render(b, ui_columns());
        viewport_item_end();
    }

    status_set_word("fanning out");
    status_begin();

    double painted = 0;
    for (;;) {
        int done = 0;
        for (int i = 0; i < n; i++)
            done += atomic_load(&b->w[i].done);
        if (done == n)
            break;

        if (session_poll_input())
            atomic_store(&aborted, 1);

        char line[128];
        snprintf(line, sizeof line, "fanning out \xc2\xb7 %d of %d back", done, n);
        status_set_word(line);
        status_tick();

        double now = now_seconds();
        if (kept && now - painted >= 0.09) {
            painted = now;
            viewport_item_update(mark);
        }

        struct timespec nap = {0, 15 * 1000 * 1000};
        nanosleep(&nap, NULL);
    }

    status_end();

    for (int i = 0; i < n; i++)
        if (b->w[i].started)
            pthread_join(b->w[i].thread, NULL);

    // Done: the whole board, with what each turn cost under it.
    b->live = 0;
    if (kept) {
        viewport_item_update(mark);
    } else {
        board_render(b, ui_columns());
        ui_flush();
    }

    int answered = 0;
    for (int i = 0; i < n; i++)
        answered += b->w[i].reply && *b->w[i].reply;
    if (!kept)
        board_free(b);
    return answered;
}
