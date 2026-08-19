#include "fanout.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "block.h"
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

struct board {
    struct worker *w;
    int            n;

    int            scroll[FAN_MAX];
    int            sel;
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

static void column_lay(struct worker *w, int width)
{
    if (w->rows_width != width) {
        w->rows_width = width;
        w->nrows = 0;
        w->laid = 0;
    }

    pthread_mutex_lock(&board_lock);
    int count = w->count;
    pthread_mutex_unlock(&board_lock);

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

static void board_row(const struct board *b, int width, const int *from, int r,
                      int erase, int pad_tail)
{
    int last = -1;
    for (int i = 0; i < b->n; i++)
        if (from[i] + r < b->w[i].nrows)
            last = i;

    if (erase)
        ui_esc(UI_ERASE_EOL);
    for (int i = 0; i < b->n; i++) {
        if (!pad_tail && i > last)
            break;
        int cell = width - (i + 1 < b->n ? 0 : 1);
        int at = from[i] + r;
        if (at >= b->w[i].nrows) {
            if (pad_tail || i < last)
                ui_pad(cell);
            continue;
        }
        const struct cell *c = &b->w[i].rows[at];
        ui_putn(c->text, c->bytes);

        ui_esc(ui_style(UI_RESET));
        if (pad_tail || i < last)
            ui_pad(cell - c->width);
    }
    ui_put("\n");
}

static void label_row(const struct board *b, int width, int closing, int erase)
{
    if (erase)
        ui_esc(UI_ERASE_EOL);
    pthread_mutex_lock(&board_lock);
    for (int i = 0; i < b->n; i++) {
        const struct worker *w = &b->w[i];
        char text[160];
        enum ui_role role = b->sel == i ? UI_ACCENT : UI_BOLD;

        if (!closing) {
            if (b->scroll[i])
                snprintf(text, sizeof text, "%s \xe2\x86\x91%d", w->name, b->scroll[i]);
            else
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
        if (i + 1 < b->n || erase)
            ui_pad(width - (i + 1 < b->n ? 0 : 1) - (int)ui_cells_n(text, bytes));
    }
    pthread_mutex_unlock(&board_lock);
    ui_put("\n");
}

static void widget_paint(struct board *b, const char *word, double elapsed, int frame)
{
    static const char *const FRAMES[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};

    int cols = ui_columns();
    int width = board_width(b, cols);
    board_lay(b, width);

    int body = tty_rows() - 2;
    if (body < 1)
        body = 1;

    int from[FAN_MAX];
    for (int i = 0; i < b->n; i++) {
        int tail = b->w[i].nrows > body ? b->w[i].nrows - body : 0;
        if (b->scroll[i] > tail)
            b->scroll[i] = tail;
        from[i] = tail - b->scroll[i];
    }

    ui_sync_begin();
    ui_esc(UI_HOME);
    label_row(b, width, 0, 1);
    for (int r = 0; r < body; r++)
        board_row(b, width, from, r, 1, 0);

    ui_esc(UI_ERASE_EOL);
    char clock[32];
    snprintf(clock, sizeof clock, "%s %.0fs \xc2\xb7 %s", FRAMES[frame % 10], elapsed, word);
    ui_esc(ui_style(UI_SPIN));
    ui_put(clock);
    ui_esc(ui_style(UI_DIM));
    ui_put("  esc interrupt \xc2\xb7 \xe2\x86\x90\xe2\x86\x92 column \xc2\xb7 "
           "\xe2\x86\x91\xe2\x86\x93/pgup scroll \xc2\xb7 end follow");
    ui_esc(ui_style(UI_RESET));
    ui_sync_end();
    ui_flush();
}

#define WIDGET_STEP 3

static int widget_keys(struct board *b, int wait_ms, int *moved)
{
    int interrupt = 0;
    int page = tty_rows() - 4;
    if (page < 1)
        page = 1;

    tty_event ev;
    for (int first = 1; tty_read(&ev, first ? wait_ms : 0); first = 0) {

        enum { KEEP, TOP, FOLLOW } jump = KEEP;
        int step = 0;

        switch (ev.key) {
        case TK_TEXT:      free(ev.text);                                    break;
        case TK_EOF:
        case TK_ESCAPE:    interrupt = 1;                                    break;
        case TK_CHAR:      interrupt |= ev.cp == 3;                          break;
        case TK_LEFT:      b->sel = b->sel < 0 ? b->n - 1 : b->sel - 1;
                           *moved = 1;                                       break;
        case TK_RIGHT:     b->sel = b->sel + 1 >= b->n ? -1 : b->sel + 1;
                           *moved = 1;                                       break;
        case TK_UP:        step = WIDGET_STEP;                               break;
        case TK_DOWN:      step = -WIDGET_STEP;                              break;
        case TK_PAGE_UP:
        case TK_WORD_LEFT: step = page;                                      break;
        case TK_PAGE_DOWN:
        case TK_WORD_RIGHT:step = -page;                                     break;
        case TK_HOME:      jump = TOP;                                       break;
        case TK_END:       jump = FOLLOW;                                    break;
        default:                                                             break;
        }

        if (jump == KEEP && !step)
            continue;
        for (int i = 0; i < b->n; i++) {
            if (b->sel >= 0 && b->sel != i)
                continue;
            int at = jump == TOP    ? b->w[i].nrows
                   : jump == FOLLOW ? 0
                                    : b->scroll[i] + step;
            b->scroll[i] = at < 0 ? 0 : at;
        }
        *moved = 1;
    }
    return interrupt;
}

static void board_print(struct board *b, int cols)
{
    int width = board_width(b, cols);

    if (width < FAN_COL_MIN) {

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
        ui_flush();
        return;
    }

    board_lay(b, width);

    memset(b->scroll, 0, sizeof b->scroll);
    b->sel = -1;

    int from[FAN_MAX] = {0};
    int height = board_height(b);

    ui_put("\n");
    label_row(b, width, 0, 0);
    for (int r = 0; r < height; r++)
        board_row(b, width, from, r, 0, 0);
    label_row(b, width, 1, 0);
    ui_put("\n");
    ui_flush();
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

    struct worker w[FAN_MAX];
    memset(w, 0, sizeof w);

    const char *spec = settings_get_str(SETTING_MUX_BACKENDS, FAN_ROSTER);
    int n = roster(w, FAN_MAX, spec);
    if (n < 1) {
        ui_error("no known backends in " SETTING_MUX_BACKENDS " (%s)", spec);
        ui_put("\n");
        ui_flush();
        return 0;
    }

    for (int i = 0; i < n; i++) {
        const char *model = session_saved_model(w[i].name);
        const char *effort = session_saved_effort(w[i].name);
        if (model)
            snprintf(w[i].model, sizeof w[i].model, "%s", model);
        if (effort)
            snprintf(w[i].effort, sizeof w[i].effort, "%s", effort);
        w[i].cwd = session_cwd(s);
        w[i].prompt = prompt;
        w[i].permission = session_permission(s);
    }

    atomic_store(&aborted, 0);
    atomic_store(&show_thinking, session_thinking(s));
    for (int i = 0; i < n; i++) {
        if (pthread_create(&w[i].thread, NULL, fan_work, &w[i]) == 0) {
            w[i].started = 1;
        } else {
            pthread_mutex_lock(&board_lock);
            w[i].state = FAN_FAIL;
            snprintf(w[i].error, sizeof w[i].error, "could not start a worker");
            log_add(&w[i], FAN_NOTE, w[i].error);
            pthread_mutex_unlock(&board_lock);
            atomic_store(&w[i].done, 1);
        }
    }

    struct board board = {.w = w, .n = n, .sel = -1};

    int widget = board_width(&board, ui_columns()) >= FAN_COL_MIN;
    if (widget) {
        ui_esc(UI_ALT_ON);
        ui_esc(UI_CURSOR_HIDE);
        ui_flush();
    } else {
        status_set_word("fanning out");
        status_begin();
    }

    double started = now_seconds();
    double painted = 0;
    for (;;) {
        int done = 0;
        for (int i = 0; i < n; i++)
            done += atomic_load(&w[i].done);
        if (done == n)
            break;

        char word[96];
        snprintf(word, sizeof word, "%d of %d back", done, n);

        double now = now_seconds();
        if (widget) {
            int moved = 0;
            if (widget_keys(&board, 15, &moved))
                atomic_store(&aborted, 1);
            now = now_seconds();
            if (moved || now - painted >= 0.09) {
                painted = now;
                widget_paint(&board, word, now - started, (int)((now - started) / 0.09));
            }
            continue;
        } else {
            if (session_poll_input())
                atomic_store(&aborted, 1);
            char line[128];
            snprintf(line, sizeof line, "fanning out \xc2\xb7 %s", word);
            status_set_word(line);
            status_tick();
        }

        struct timespec nap = {0, 15 * 1000 * 1000};
        nanosleep(&nap, NULL);
    }

    if (widget) {
        ui_esc(UI_ALT_OFF);
        ui_esc(UI_CURSOR_SHOW);
        ui_flush();
        block_forget();
    } else {
        status_end();
    }

    for (int i = 0; i < n; i++)
        if (w[i].started)
            pthread_join(w[i].thread, NULL);

    board_print(&board, ui_columns());

    int answered = 0;
    for (int i = 0; i < n; i++) {
        answered += w[i].reply && *w[i].reply;
        free(w[i].reply);
        free(w[i].rows);
        log_free(&w[i]);
    }
    return answered;
}
