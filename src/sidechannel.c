#include "sidechannel.h"

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "chrome.h"
#include "md.h"
#include "scrollback.h"
#include "session.h"
#include "sessionfork.h"
#include "status.h"
#include "text.h"
#include "ui.h"
#include "viewport.h"
#include "vendor/cJSON.h"

#define SIDE_MAX 4

// The asker never sees a prompt from this turn, so a question back is a dead
// end on the screen.
#define BTW_PREAMBLE                                                            \
    "Answer the question below as an aside in a conversation you are not part " \
    "of. The person asking cannot reply to you: you get one answer and the "    \
    "exchange ends there. Do not ask follow-up questions, do not offer to do "  \
    "more, and do not ask what they want next. Be brief.\n\n"

struct stream {
    int    fd;
    char  *buf;
    size_t len, cap;
};

// An answered question: one entry, so the answer stays with what was asked.
struct btw {
    char *question;
    char *answer;
    int   failed;
    int   gap;                  /* owed a blank row above */
};

struct side {
    pid_t         pid;
    struct stream out, err;
    char         *question;
};

static struct side slots[SIDE_MAX];

static void stream_free(struct stream *s)
{
    if (s->fd >= 0)
        close(s->fd);
    free(s->buf);
    s->fd = -1;
    s->buf = NULL;
    s->len = s->cap = 0;
}

static void slot_free(struct side *c)
{
    stream_free(&c->out);
    stream_free(&c->err);
    free(c->question);
    c->question = NULL;
    c->pid = 0;
}

static const char *const SPIN[] = {"\u280b", "\u2819", "\u2839", "\u2838", "\u283c",
                                   "\u2834", "\u2826", "\u2827", "\u2807", "\u280f"};
#define SPIN_N ((int)(sizeof SPIN / sizeof *SPIN))

#define BTW_DONE "\u2713 "
#define BTW_FAIL "\u00d7 "

// Every row carries the bar, which tells a side answer from the main turn.
static void bar_rows(const char *painted, enum ui_role role)
{
    size_t len = strlen(painted);
    while (len && painted[len - 1] == '\n')
        len--;

    const char *p = painted;
    const char *end = painted + len;
    while (p <= end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);

        ui_esc(ui_style(role));
        ui_put(UI_BAR);
        ui_esc(ui_style(UI_RESET));
        if (n) {
            ui_put(" ");
            ui_putn(p, n);
            ui_esc(ui_style(UI_RESET));
        }
        ui_put("\n");

        if (!nl)
            break;
        p = nl + 1;
    }
}

static char *btw_encode(void *ud)
{
    const struct btw *b = ud;
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "question", b->question ? b->question : "");
    cJSON_AddStringToObject(o, "answer", b->answer ? b->answer : "");
    cJSON_AddNumberToObject(o, "failed", b->failed);
    cJSON_AddNumberToObject(o, "gap", b->gap);
    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return out;
}

static void btw_render(void *ud, int cols)
{
    const struct btw *b = ud;
    (void)cols;

    char mark[16];
    snprintf(mark, sizeof mark, "%s", b->failed ? BTW_FAIL : BTW_DONE);

    // Settled when the entry was made: asking now would answer about a
    // transcript this entry is already in.
    if (b->gap)
        ui_put("\n");

    int budget = ui_columns() - 5;
    struct ui_wrap w = {0};
    w.budget = (size_t)(budget > 4 ? budget : 4);
    w.gutter = UI_BAR " ";
    w.mark = mark;
    w.role = UI_SIDE;
    w.erase = 1;
    w.paint_empty = 1;
    ui_wrap_paint(b->question, &w);

    if (!b->answer) {
        ui_put("\n");
        return;
    }

    int inner = ui_columns() - 2;
    ui_capture_begin(inner > 8 ? inner : 8);
    if (b->failed)
        ui_wrapped(b->answer, 0, UI_DIM);
    else
        md_render(b->answer, 0);
    char *painted = ui_capture_end();
    if (painted) {
        bar_rows(painted, UI_BRAND);
        free(painted);
    }
    ui_put("\n");
}

static void btw_free(void *ud)
{
    struct btw *b = ud;
    free(b->question);
    free(b->answer);
    free(b);
}

// A question still waiting is chrome: it stays on screen instead of scrolling
// away with the main turn. Answered, it becomes transcript.
static int    spin_frame;
static double spun_at;

int sidechannel_rows(void)
{
    int n = 0;
    for (int i = 0; i < SIDE_MAX; i++)
        if (slots[i].pid && slots[i].question)
            n++;
    return n;
}

void sidechannel_paint(int budget)
{
    for (int i = 0; i < SIDE_MAX && budget > 0; i++) {
        if (!slots[i].pid || !slots[i].question)
            continue;

        char flat[2048];
        text_one_line(slots[i].question, flat, sizeof flat);

        int room = ui_columns() - 6;
        if (room < 8)
            room = 8;
        size_t fit = ui_fit_visible(flat, strlen(flat), (size_t)room);

        ui_esc(ui_style(UI_SIDE));
        ui_esc(UI_ERASE_EOL);
        ui_put(UI_BAR);
        ui_put(" ");
        ui_put(SPIN[spin_frame % SPIN_N]);
        ui_put(" ");
        ui_putn(flat, fit);
        if (flat[fit])
            ui_put("\u2026");
        ui_esc(ui_style(UI_RESET));
        ui_put("\n");
        budget--;
    }
}

// Advanced on the clock, not on the call: the abort check calls this at the
// driver's poll rate, far faster than a frame.
void sidechannel_tick(void)
{
    if (!sidechannel_rows())
        return;

    if (!spin_advance(&spin_frame, &spun_at))
        return;

    // Asked for directly: status.c only paints while a main turn runs.
    chrome_paint();
}

static void slot_init(struct side *c)
{
    memset(c, 0, sizeof *c);
    c->out.fd = c->err.fd = -1;
}

static struct side *free_slot(void)
{
    for (int i = 0; i < SIDE_MAX; i++)
        if (!slots[i].pid)
            return &slots[i];
    return NULL;
}

// A non-interactive mux resuming this conversation with --fork-session: it
// reads the shared context but writes its turn to a session of its own.
static int spawn(struct side *c, const struct session *s, const char *prompt)
{
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0)
        return 0;
    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return 0;
    }

    char *argv[24];
    int n = 0;
    argv[n++] = (char *)sessionfork_program();
    argv[n++] = "-b";
    argv[n++] = (char *)session_backend(s);
    const char *cwd = session_cwd(s);
    if (cwd && *cwd) {
        argv[n++] = "-C";
        argv[n++] = (char *)cwd;
    }
    argv[n++] = "--session";
    argv[n++] = (char *)session_id(s);
    argv[n++] = "--fork";
    const char *model = session_model(s);
    if (model && strcmp(model, "default") != 0) {
        argv[n++] = "-m";
        argv[n++] = (char *)model;
    }
    const char *effort = session_effort(s);
    if (effort && strcmp(effort, "default") != 0) {
        argv[n++] = "-e";
        argv[n++] = (char *)effort;
    }
    argv[n++] = (char *)prompt;
    argv[n] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return 0;
    }
    if (pid == 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        if (out_pipe[1] != STDOUT_FILENO)
            close(out_pipe[1]);
        if (err_pipe[1] != STDERR_FILENO)
            close(err_pipe[1]);
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            if (null != STDIN_FILENO)
                close(null);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);
    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);
    c->pid = pid;
    c->out.fd = out_pipe[0];
    c->err.fd = err_pipe[0];
    return 1;
}

int sidechannel_start(const struct session *s, const char *prompt, const char *label)
{
    if (!prompt || !*prompt)
        return 0;
    if (!label || !*label)
        label = prompt;

    if (!session_can_resume(s)) {
        ui_error("%s cannot fork a conversation, so /btw has nothing to run in",
                 session_backend(s));
        ui_put("\n");
        return 0;
    }
    const char *id = session_id(s);
    if (!id || !*id) {
        ui_error("nothing to fork yet — send a message first");
        ui_put("\n");
        return 0;
    }

    struct side *c = free_slot();
    if (!c) {
        ui_error("already running %d side turns", SIDE_MAX);
        ui_put("\n");
        return 0;
    }

    size_t want = sizeof BTW_PREAMBLE + strlen(prompt);
    char  *asked = malloc(want);
    if (!asked)
        return 0;
    snprintf(asked, want, "%s%s", BTW_PREAMBLE, prompt);

    slot_init(c);
    if (!spawn(c, s, asked)) {
        slot_free(c);
        free(asked);
        ui_error("could not start the side turn");
        ui_put("\n");
        return 0;
    }
    free(asked);

    c->question = strdup(label);
    chrome_paint();
    return 1;
}

int sidechannel_fds(int *out, int max)
{
    int n = 0;
    for (int i = 0; i < SIDE_MAX; i++) {
        if (!slots[i].pid)
            continue;
        if (slots[i].out.fd >= 0 && n < max)
            out[n++] = slots[i].out.fd;
        if (slots[i].err.fd >= 0 && n < max)
            out[n++] = slots[i].err.fd;
    }
    return n;
}

int sidechannel_busy(void)
{
    for (int i = 0; i < SIDE_MAX; i++)
        if (slots[i].pid)
            return 1;
    return 0;
}

static char *trimmed(struct stream *s)
{
    if (!s->buf)
        return NULL;
    char *text = s->buf;
    while (*text == '\n' || *text == ' ')
        text++;
    size_t end = strlen(text);
    while (end && (text[end - 1] == '\n' || text[end - 1] == ' '))
        text[--end] = '\0';
    return *text ? text : NULL;
}

static void emit(struct side *c, int status)
{
    char *reply = trimmed(&c->out);
    char *why = trimmed(&c->err);

    char note[256];
    const char *answer = reply;
    int failed = 0;
    if (!answer) {
        failed = 1;
        if (why) {
            answer = why;
        } else if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            snprintf(note, sizeof note, "could not run %s for the side turn",
                     sessionfork_program());
            answer = note;
        } else {
            snprintf(note, sizeof note, "the side turn produced nothing (exit %d)",
                     WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            answer = note;
        }
    }

    struct btw *b = calloc(1, sizeof *b);
    if (!b)
        return;
    b->question = strdup(c->question ? c->question : "");
    b->answer = strdup(answer);
    b->failed = failed;
    b->gap = !viewport_ends_blank();
    if (!b->question || !b->answer) {
        btw_free(b);
        return;
    }

    unsigned mark = viewport_item_begin(btw_render, b, btw_free);
    btw_render(b, ui_columns());
    viewport_item_end();
    viewport_item_persist(mark, SIDECHANNEL_BTW_KIND, btw_encode);
    ui_flush();
}

void sidechannel_btw_load(const cJSON *st)
{
    struct btw *b = calloc(1, sizeof *b);
    if (!b)
        return;
    b->question = strdup(scrollback_str(st, "question"));
    b->answer = strdup(scrollback_str(st, "answer"));
    b->failed = scrollback_int(st, "failed");
    b->gap = scrollback_int(st, "gap");
    if (!b->question || !b->answer) {
        btw_free(b);
        return;
    }

    unsigned mark = viewport_item_begin(btw_render, b, btw_free);
    btw_render(b, ui_columns());
    viewport_item_end();
    viewport_item_persist(mark, SIDECHANNEL_BTW_KIND, btw_encode);
}

// Returns nonzero while the stream is still open.
static int drain(struct stream *s)
{
    if (s->fd < 0)
        return 0;
    for (;;) {
        if (s->len + 8192 + 1 > s->cap) {
            size_t want = s->cap ? s->cap * 2 : 16384;
            while (want < s->len + 8192 + 1)
                want *= 2;
            char *grown = realloc(s->buf, want);
            if (!grown)
                break;
            s->buf = grown;
            s->cap = want;
        }
        ssize_t r = read(s->fd, s->buf + s->len, 8192);
        if (r > 0) {
            s->len += (size_t)r;
            s->buf[s->len] = '\0';
            continue;
        }
        if (r == 0)
            break;                      /* the child is gone: real EOF */
        if (errno == EINTR)
            continue;
        // SIGWINCH and SIGURG carry no SA_RESTART, so EINTR here is routine
        // and is not the child finishing.
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 1;
        break;
    }
    close(s->fd);
    s->fd = -1;
    return 0;
}

void sidechannel_poll(void)
{
    for (int i = 0; i < SIDE_MAX; i++) {
        struct side *c = &slots[i];
        if (!c->pid)
            continue;
        int live = drain(&c->out);
        live |= drain(&c->err);
        if (live)
            continue;

        int status = 0;
        while (waitpid(c->pid, &status, 0) < 0 && errno == EINTR)
            ;

        // Released before anything that might paint: a paint reaches back
        // into this poll through the busy check, and a slot still holding a
        // reaped pid emits again at every level it is re-entered.
        struct side done = *c;
        slot_init(c);

        emit(&done, status);

        stream_free(&done.out);
        stream_free(&done.err);
        free(done.question);

        chrome_paint();
    }
}

void sidechannel_close_all(void)
{
    for (int i = 0; i < SIDE_MAX; i++) {
        if (!slots[i].pid)
            continue;
        kill(slots[i].pid, SIGTERM);
        int status = 0;
        while (waitpid(slots[i].pid, &status, 0) < 0 && errno == EINTR)
            ;
        slot_free(&slots[i]);
    }
}
