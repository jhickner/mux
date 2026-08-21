#include "fanout.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "filediff.h"
#include "muxcfg.h"
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

#define FAN_MAX MUX_MAX

// The table needs room for a name beside a readable answer; under that it
// falls back to one board after another.
#define FAN_LABEL_MIN 8
#define FAN_LABEL_MAX 22
#define FAN_BODY_MIN  24

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

// One laid-out line of a cell, pointing into an entry's painted text.
struct cell {
    const char *text;
    size_t      bytes;
    int         width;
};

struct rowbuf {
    struct cell *v;
    int          n;
    int          cap;
};

struct worker {
    char name[32];
    char model[128];
    char effort[32];
    char system[MUX_PROMPT];
    char resolved[128];

    const char *cwd;
    const char *prompt;
    const char *permission;

    double      began;
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

    struct rowbuf rows;
    int           rows_width;
    int           laid;
};

// One transcript entry, redrawn in place while the turns run.
struct board {
    struct worker w[FAN_MAX];
    int           n;
    int           live;         /* still running: only the tail of a cell shows */

    char          config[MUX_NAME];
    char         *prompt;
    char         *head;         /* the prompt, painted at the body width */
    int           head_width;
    struct rowbuf head_rows;
};

static atomic_int      aborted;
static atomic_int      show_thinking;
static pthread_mutex_t board_lock = PTHREAD_MUTEX_INITIALIZER;

static int fan_aborted(void) { return atomic_load(&aborted); }

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
    w->count = w->cap = w->laid = w->rows.n = 0;
}

static void fan_event(void *ud, const backend_event *ev)
{
    struct worker *w = ud;

    pthread_mutex_lock(&board_lock);
    switch (ev->kind) {

    case BACKEND_EV_CWD:
    case BACKEND_EV_TRUST:
    case BACKEND_EV_WARNING:
    case BACKEND_EV_TASK:
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

static void rowbuf_add(struct rowbuf *rb, const char *text, size_t bytes, int width)
{
    if (ui_cells_visible(text, bytes) > (size_t)width)
        bytes = ui_fit_visible(text, bytes, (size_t)width);

    if (rb->n == rb->cap) {
        int cap = rb->cap ? rb->cap * 2 : 64;
        struct cell *grown = realloc(rb->v, (size_t)cap * sizeof *grown);
        if (!grown)
            return;
        rb->v = grown;
        rb->cap = cap;
    }
    rb->v[rb->n++] = (struct cell){text, bytes, (int)ui_cells_visible(text, bytes)};
}

static void rowbuf_split(struct rowbuf *rb, const char *painted, int width)
{
    const char *p = painted;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        rowbuf_add(rb, p, nl ? (size_t)(nl - p) : strlen(p), width);
        if (!nl)
            break;
        p = nl + 1;
    }
}

// Called with the board locked: the workers are still writing to their logs.
static void cell_lay(struct worker *w, int width)
{
    if (w->rows_width != width) {
        w->rows_width = width;
        w->rows.n = 0;
        w->laid = 0;
    }

    int count = w->count;

    for (; w->laid < count; w->laid++)
        rowbuf_split(&w->rows, entry_painted(&w->log[w->laid], width), width);
}

static void head_lay(struct board *b, int width)
{
    if (b->head && b->head_width == width)
        return;

    free(b->head);
    b->head_rows.n = 0;
    b->head_width = width;
    ui_capture_begin(width);
    ui_wrapped(b->prompt ? b->prompt : "", 0, UI_ECHO);
    b->head = ui_capture_end();
    rowbuf_split(&b->head_rows, b->head, width);
}

static void board_lay(struct board *b, int width)
{
    head_lay(b, width);
    for (int i = 0; i < b->n; i++)
        cell_lay(&b->w[i], width);
}

// The y axis: what answered, and how it was asked to.
static int worker_labels(const struct worker *w, int live, char out[4][64],
                         enum ui_role *role)
{
    int n = 0;

    role[n] = w->state == FAN_FAIL ? UI_ERROR : UI_BOLD;
    snprintf(out[n++], 64, "%s", w->name);

    const char *model = *w->resolved ? w->resolved : *w->model ? w->model : "default";
    if (!strncmp(w->name, "claude", 6) && !strncmp(model, "claude-", 7) && model[7])
        model += 7;
    role[n] = UI_DIM;
    snprintf(out[n++], 64, "%s", model);

    if (*w->effort && strcmp(w->effort, "default")) {
        role[n] = UI_DIM;
        snprintf(out[n++], 64, "%s", w->effort);
    }

    if (w->began) {
        double secs = w->state == FAN_WORK ? now_seconds() - w->began : w->secs;
        role[n] = live && w->state == FAN_WORK ? UI_ACCENT : UI_DIM;
        snprintf(out[n++], 64, "%.1fs", secs);
    }
    return n;
}

static int label_width(const struct board *b, int live, int budget)
{
    int width = FAN_LABEL_MIN;

    for (int i = 0; i < b->n; i++) {
        char         text[4][64];
        enum ui_role role[4];
        int          n = worker_labels(&b->w[i], live, text, role);
        for (int r = 0; r < n; r++) {
            int cells = (int)ui_cells(text[r]);
            if (cells > width)
                width = cells;
        }
    }
    if (width > FAN_LABEL_MAX)
        width = FAN_LABEL_MAX;
    return width > budget ? budget : width;
}

static void rule(int labelw, int bodyw, const char *left, const char *mid,
                 const char *right)
{
    ui_esc(ui_style(UI_CHROME));
    ui_put(left);
    for (int i = 0; i < labelw + 2; i++)
        ui_put("\xe2\x94\x80");
    ui_put(mid);
    for (int i = 0; i < bodyw + 2; i++)
        ui_put("\xe2\x94\x80");
    ui_put(right);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static void edge(void)
{
    ui_esc(ui_style(UI_CHROME));
    ui_put("\xe2\x94\x82");
    ui_esc(ui_style(UI_RESET));
}

static void table_row(int labelw, int bodyw, const char *label, enum ui_role role,
                      const struct cell *c)
{
    edge();
    ui_put(" ");
    if (label && *label) {
        size_t bytes = ui_fit_bytes(label, (size_t)labelw);
        ui_esc(ui_style(role));
        ui_putn(label, bytes);
        ui_esc(ui_style(UI_RESET));
        ui_pad(labelw - (int)ui_cells_n(label, bytes));
    } else {
        ui_pad(labelw);
    }
    ui_put(" ");

    edge();
    ui_put(" ");
    if (c) {
        ui_putn(c->text, c->bytes);
        ui_esc(ui_style(UI_RESET));
        ui_pad(bodyw - c->width);
    } else {
        ui_pad(bodyw);
    }
    ui_put(" ");
    edge();
    ui_put("\n");
}

static void head_block(struct board *b, int labelw, int bodyw)
{
    int rows = b->head_rows.n > 0 ? b->head_rows.n : 1;

    for (int r = 0; r < rows; r++)
        table_row(labelw, bodyw, r ? NULL : b->config, UI_DIM,
                  r < b->head_rows.n ? &b->head_rows.v[r] : NULL);
}

// What the row was told to always do, kept at the top of its cell: the answer
// below it scrolls, this does not.
static int standing_row(const struct worker *w, int bodyw, char *out, size_t cap,
                        struct cell *pin)
{
    if (!*w->system)
        return 0;

    char text[MUX_PROMPT + 8];
    snprintf(text, sizeof text, "\xe2\x80\xba %s", w->system);

    size_t bytes = ui_fit_bytes(text, (size_t)(bodyw > 1 ? bodyw - 1 : 1));
    snprintf(out, cap, "%s%.*s%s%s", ui_style(UI_DIM), (int)bytes, text,
             text[bytes] ? "\xe2\x80\xa6" : "", ui_style(UI_RESET));

    size_t n = strlen(out);
    *pin = (struct cell){out, n, (int)ui_cells_visible(out, n)};
    return 1;
}

static void worker_block(struct worker *w, int live, int labelw, int bodyw, int cap)
{
    char         label[4][64];
    enum ui_role role[4];
    int          labels = worker_labels(w, live, label, role);

    char        note[MUX_PROMPT + 64];
    struct cell pin;
    int         pinned = standing_row(w, bodyw, note, sizeof note, &pin);

    int from = 0, rows = w->rows.n;
    if (cap && rows > cap - pinned) {
        from = rows - (cap - pinned);
        rows = cap - pinned;
    }

    int height = rows + pinned > labels ? rows + pinned : labels;
    if (height < 1)
        height = 1;

    for (int r = 0; r < height; r++) {
        const struct cell *c = NULL;
        if (pinned && r == 0)
            c = &pin;
        else if (r - pinned < rows)
            c = &w->rows.v[from + r - pinned];

        table_row(labelw, bodyw, r < labels ? label[r] : NULL,
                  r < labels ? role[r] : UI_DIM, c);
    }
}

// Too narrow for a table: one board after another, each under its own heading.
static void board_stacked(struct board *b, int cols)
{
    for (int i = 0; i < b->n; i++) {
        struct worker *w = &b->w[i];
        char           label[4][64];
        enum ui_role   role[4];
        int            labels = worker_labels(w, b->live, label, role);

        ui_bar(ui_style(UI_CHROME), "%s \xc2\xb7 %s", label[0],
               labels > 1 ? label[1] : "");
        ui_put("\n");
        if (*w->system) {
            char note[MUX_PROMPT + 64];
            struct cell pin;
            if (standing_row(w, cols, note, sizeof note, &pin)) {
                ui_putn(pin.text, pin.bytes);
                ui_put("\n");
            }
        }
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

    // The last column stays clear: a glyph there wraps on some terminals.
    int total = cols - 1;
    int labelw = label_width(b, b->live, total / 3);
    int bodyw = total - labelw - 7;

    if (bodyw < FAN_BODY_MIN || labelw < FAN_LABEL_MIN) {
        board_stacked(b, cols);
        pthread_mutex_unlock(&board_lock);
        return;
    }

    board_lay(b, bodyw);

    // While the turns run, each row shows only its tail, so a long answer
    // cannot push the rest of the transcript off screen.
    int cap = 0;
    if (b->live) {
        cap = live_rows() / b->n;
        if (cap < 3)
            cap = 3;
    }

    rule(labelw, bodyw, "\xe2\x94\x8c", "\xe2\x94\xac", "\xe2\x94\x90");
    head_block(b, labelw, bodyw);
    for (int i = 0; i < b->n; i++) {
        rule(labelw, bodyw, "\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4");
        worker_block(&b->w[i], b->live, labelw, bodyw, cap);
    }
    rule(labelw, bodyw, "\xe2\x94\x94", "\xe2\x94\xb4", "\xe2\x94\x98");

    pthread_mutex_unlock(&board_lock);
}

static void board_free(void *ud)
{
    struct board *b = ud;
    for (int i = 0; i < b->n; i++) {
        free(b->w[i].reply);
        free(b->w[i].rows.v);
        log_free(&b->w[i]);
    }
    free(b->head_rows.v);
    free(b->head);
    free(b->prompt);
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
    o.system = *w->system ? w->system : NULL;
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

    pthread_mutex_lock(&board_lock);
    double started = w->began = now_seconds();
    pthread_mutex_unlock(&board_lock);

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

int fanout_run(struct session *s, const char *prompt)
{
    if (!prompt || !*prompt)
        return 0;

    // The board outlives this call: the transcript keeps it.
    struct board *b = calloc(1, sizeof *b);
    if (!b)
        return 0;

    struct mux_spec spec[FAN_MAX];
    int             n = muxcfg_load(spec, FAN_MAX);
    if (n < 1) {
        free(b);
        ui_error("the mux matrix is empty \xe2\x80\x94 /mux config to fill it in");
        ui_put("\n");
        ui_flush();
        return 0;
    }
    b->n = n;
    b->live = 1;
    b->prompt = strdup(prompt);
    snprintf(b->config, sizeof b->config, "%s", muxcfg_active());

    for (int i = 0; i < n; i++) {
        struct worker *w = &b->w[i];
        snprintf(w->name, sizeof w->name, "%s", spec[i].backend);
        snprintf(w->model, sizeof w->model, "%s", spec[i].model);
        snprintf(w->effort, sizeof w->effort, "%s", spec[i].effort);
        snprintf(w->system, sizeof w->system, "%s", spec[i].prompt);
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
        mark = viewport_item_begin(board_render, b, board_free, 1, 1);
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
