#include "sidechannel.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "md.h"
#include "session.h"
#include "sessionfork.h"
#include "status.h"
#include "ui.h"

#define SIDE_MAX 4

struct stream {
    int    fd;
    char  *buf;
    size_t len, cap;
};

struct side {
    pid_t         pid;
    struct stream out, err;
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
    c->pid = 0;
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

// The child is a non-interactive mux resuming this conversation with
// --fork-session, so it reads the shared context but writes its turn to a
// session of its own, leaving the transcript we are still using alone.
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

int sidechannel_start(const struct session *s, const char *prompt)
{
    if (!prompt || !*prompt)
        return 0;

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

    slot_init(c);
    if (!spawn(c, s, prompt)) {
        slot_free(c);
        ui_error("could not start the side turn");
        ui_put("\n");
        return 0;
    }
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

// A side reply lands wherever the main turn happens to be, so it is drawn as a
// callout: every row carries the brand bar, which is what tells the two apart.
static void callout(const char *painted)
{
    size_t len = strlen(painted);
    while (len && painted[len - 1] == '\n')
        len--;

    const char *p = painted;
    const char *end = painted + len;
    while (p <= end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);

        ui_esc(ui_style(UI_BRAND));
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
    status_pause();

    char *reply = trimmed(&c->out);
    char *why = trimmed(&c->err);

    int cols = ui_columns() - 2;
    ui_capture_begin(cols > 8 ? cols : 8);
    if (reply) {
        md_render(reply, 0);
    } else if (why) {
        // Only the child's own diagnosis explains an empty side turn, so it is
        // worth more here than a generic failure line.
        ui_error("the side turn failed");
        ui_wrapped(why, 0, UI_DIM);
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        ui_error("could not run %s for the side turn", sessionfork_program());
    } else {
        ui_error("the side turn produced nothing (exit %d)",
                 WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    char *painted = ui_capture_end();

    if (painted) {
        callout(painted);
        free(painted);
    }
    ui_put("\n");
    ui_flush();

    status_resume();
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
        // mux's SIGWINCH and SIGURG handlers carry no SA_RESTART, and this runs
        // from the turn's abort check, so an interrupted read is routine and
        // must not be mistaken for the child finishing.
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
        emit(c, status);
        slot_free(c);
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
