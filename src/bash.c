#include "bash.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <util.h>
#else
#include <pty.h>
#endif

#include "block.h"
#include "text.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"

#define RAW_CAP  (1u << 20)
#define CTX_CAP  16384
#define CTX_KEEP 6000
#define CTX_TOTAL (1u << 18)

struct buf {
    char  *data;
    size_t len, cap;
    int    full;
};

static int buf_add(struct buf *b, const char *src, size_t n, size_t cap_bytes)
{
    if (b->len + n > cap_bytes) {
        n = b->len < cap_bytes ? cap_bytes - b->len : 0;
        b->full = 1;
        if (!n)
            return 1;
    }
    if (b->len + n + 1 > b->cap) {
        size_t want = b->cap ? b->cap : 4096;
        while (want < b->len + n + 1)
            want *= 2;
        char *p = realloc(b->data, want);
        if (!p)
            return 0;
        b->data = p;
        b->cap = want;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

static const char *bash_body(const char *line)
{
    if (!line || *line != '!')
        return NULL;
    const char *cmd = line + 1;
    while (*cmd == ' ' || *cmd == '\t')
        cmd++;
    return *cmd ? cmd : NULL;
}

int bash_is_command(const char *line)
{
    return bash_body(line) != NULL;
}

// Tabs become the spaces they stood for. A tab is one byte but up to eight
// columns, so anything measuring the text afterwards — a wrap, a width — would
// think the line far shorter than the screen showed it.
#define TAB_STOP 8

static char *plain_text(const char *raw, size_t len)
{
    size_t tabs = 0;
    for (size_t i = 0; i < len; i++)
        if (raw[i] == '\t')
            tabs++;

    char *out = malloc(len + tabs * (TAB_STOP - 1) + 1);
    if (!out)
        return NULL;
    size_t o = 0, line_start = 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)raw[i];

        if (c == 0x1b) {
            if (i + 1 >= len)
                break;
            unsigned char k = (unsigned char)raw[++i];
            if (k == '[') {
                while (++i < len) {
                    unsigned char f = (unsigned char)raw[i];
                    if (f >= 0x40 && f <= 0x7e)
                        break;
                }
            } else if (k == ']' || k == 'P' || k == 'X' || k == '^' || k == '_') {

                while (++i < len) {
                    if ((unsigned char)raw[i] == 0x07)
                        break;
                    if ((unsigned char)raw[i] == 0x1b && i + 1 < len && raw[i + 1] == '\\') {
                        i++;
                        break;
                    }
                }
            } else if (k == '(' || k == ')' || k == '#' || k == '%') {
                i++;
            }
            continue;
        }

        if (c == '\r') {
            if (i + 1 < len && raw[i + 1] == '\n')
                continue;
            o = line_start;
            continue;
        }
        if (c == '\n') {
            out[o++] = '\n';
            line_start = o;
            continue;
        }
        if (c == '\b') {
            if (o > line_start)
                o--;
            continue;
        }
        if (c == '\t') {
            size_t at = o - line_start;
            size_t to = (at / TAB_STOP + 1) * TAB_STOP;
            while (at++ < to)
                out[o++] = ' ';
            continue;
        }
        if (c >= 0x20)
            out[o++] = (char)c;
    }

    while (o && (out[o - 1] == '\n' || out[o - 1] == ' ' || out[o - 1] == '\t'))
        o--;
    out[o] = '\0';
    return out;
}

static char *elide(char *text)
{
    size_t len = strlen(text);
    if (len <= CTX_CAP)
        return text;

    size_t head = CTX_KEEP;
    while (head < len && text[head] != '\n')
        head++;
    size_t tail = len - CTX_KEEP;
    while (tail < len && text[tail] != '\n')
        tail++;
    if (tail <= head)
        return text;

    size_t size = head + (len - tail) + 64;
    char *out = malloc(size);
    if (!out)
        return text;
    int n = snprintf(out, size, "%.*s\n… %zu bytes elided …\n", (int)head, text,
                     tail - head);
    if (n < 0 || (size_t)n >= size) {
        free(out);
        return text;
    }

    if (tail < len)
        memcpy(out + n, text + tail + 1, len - tail);
    free(text);
    return out;
}

static struct buf ctx;

static void bash_context_clear(void)
{
    free(ctx.data);
    memset(&ctx, 0, sizeof ctx);
}

char *bash_take_context(void)
{
    char *out = ctx.len ? strdup(ctx.data) : NULL;
    bash_context_clear();
    return out;
}

static int add_formatted(struct buf *b, const char *text, int n)
{
    if (n < 0)
        return 0;
    size_t len = (size_t)n < strlen(text) ? (size_t)n : strlen(text);
    return buf_add(b, text, len, CTX_TOTAL);
}

static void context_add(const char *cmd, const char *out, int status)
{
    char head[512];
    int  n;
    int  ok = 1;
    if (!ctx.len) {
        const char *lead = "The user ran a shell command in the terminal. Respond to it and its "
                           "output; they can see the output already, so do not repeat it back.\n\n";
        ok = buf_add(&ctx, lead, strlen(lead), CTX_TOTAL);
    }
    n = snprintf(head, sizeof head, "<bash-input>%.400s</bash-input>\n", cmd);
    ok = add_formatted(&ctx, head, n) && ok;

    if (WIFSIGNALED(status))
        n = snprintf(head, sizeof head, "<bash-output signal=\"%d\">\n", WTERMSIG(status));
    else
        n = snprintf(head, sizeof head, "<bash-output exit=\"%d\">\n",
                     WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    ok = add_formatted(&ctx, head, n) && ok;
    ok = buf_add(&ctx, out, strlen(out), CTX_TOTAL) && ok;
    ok = buf_add(&ctx, "\n</bash-output>\n\n", 17, CTX_TOTAL) && ok;

    if (!ok)
        bash_context_clear();
}

// What a shell command left behind. The command runs on the terminal's own
// screen, which mux gives back for the duration, so nothing of it survives the
// return to the transcript unless the transcript is told. The command itself
// is not repeated here: the echo of what was typed is already above it.
struct ran {
    char *out;
};

static void ran_free(void *ud)
{
    struct ran *r = ud;
    free(r->out);
    free(r);
}

static void ran_render(void *ud, int cols)
{
    const struct ran *r = ud;
    (void)cols;

    // The output starts on the row under the command that was echoed. What
    // stands the next thing off it is the blank row printed after the run.
    if (r->out)
        ui_wrapped(r->out, 4, UI_DIM);
}

// The entry opens empty and is amended as the command writes, so its output
// arrives where it will stay rather than being blitted onto the screen and
// then printed again.
static unsigned keep_ran(void)
{
    struct ran *r = calloc(1, sizeof *r);
    if (!r)
        return 0;

    unsigned mark = viewport_item_begin(ran_render, r, ran_free);
    ran_render(r, ui_columns());
    viewport_item_end();
    return mark;
}

static void ran_set(unsigned mark, const char *out)
{
    struct ran *r = viewport_item_data(mark);
    if (!r)
        return;
    char *copy = out && *out ? strdup(out) : NULL;
    free(r->out);
    r->out = copy;
    viewport_item_update(mark);
}

// A command that asks for the alternate screen wants the terminal to itself.
// Nothing short of handing it over will do, so the transcript stops here and
// mux gets out of the way until the command is done.
static int wants_screen(const char *p, size_t n)
{
    static const char *const ASK[] = {"\x1b[?1049h", "\x1b[?1047h", "\x1b[?47h"};
    for (size_t i = 0; i < sizeof ASK / sizeof *ASK; i++) {
        size_t len = strlen(ASK[i]);
        if (n < len)
            continue;
        for (size_t k = 0; k + len <= n; k++)
            if (memcmp(p + k, ASK[i], len) == 0)
                return 1;
    }
    return 0;
}

// Stand down: the terminal's own screen, in the mode the command expects.
static void hand_over(int was_raw)
{
    viewport_suspend();
    if (was_raw) {
        ui_raw(0);
        ui_cursor_restore();
        ui_esc("\x1b[?2004l");
    }
    ui_flush();
}

static void write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        p += w;
        n -= (size_t)w;
    }
}

static void set_winsize(int fd)
{
    struct winsize ws = {0};
    ws.ws_col = (unsigned short)tty_columns();
    ws.ws_row = (unsigned short)tty_rows();
    ioctl(fd, TIOCSWINSZ, &ws);
}

void bash_run(const char *line)
{
    const char *cmd = bash_body(line);
    if (!cmd)
        return;

    struct termios cooked;
    if (tty_cooked_termios(&cooked) != 0) {
        ui_error("could not read the terminal mode");
        return;
    }
    struct winsize ws = {0};
    ws.ws_col = (unsigned short)tty_columns();
    ws.ws_row = (unsigned short)tty_rows();

    int was_raw = tty_is_raw();

    struct sigaction ignore = {0}, old_int, old_quit;
    sigemptyset(&ignore.sa_mask);
    ignore.sa_flags = 0;
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGINT, &ignore, &old_int);
    sigaction(SIGQUIT, &ignore, &old_quit);

    int   master = -1;
    int   status = 0;
    pid_t pid = forkpty(&master, NULL, &cooked, &ws);
    if (pid == 0) {
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGQUIT, &old_quit, NULL);
        const char *sh = getenv("SHELL");
        if (!sh || !*sh)
            sh = "/bin/sh";
        execl(sh, sh, "-c", cmd, (char *)NULL);
        _exit(127);
    }

    // The command writes into an entry of its own. Only one that asks for the
    // whole screen gets it, and then mux stands down for as long as it runs.
    unsigned mark = viewport_active() ? keep_ran() : 0;
    int      handed = !mark;

    struct buf raw = {0};
    if (pid > 0) {
        char   chunk[8192];
        size_t n = tty_take_pending(chunk, sizeof chunk);
        if (n)
            write_all(master, chunk, n);

        unsigned epoch = tty_resize_epoch();
        int      stdin_open = 1;
        double   shown = 0;
        for (;;) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(master, &fds);
            if (stdin_open)
                FD_SET(STDIN_FILENO, &fds);
            int r = select(master + 1, &fds, NULL, NULL, NULL);
            if (r < 0) {
                if (errno == EINTR) {
                    if (tty_resize_epoch() != epoch) {
                        epoch = tty_resize_epoch();
                        set_winsize(master);
                    }
                    continue;
                }
                break;
            }
            if (FD_ISSET(master, &fds)) {
                ssize_t got = read(master, chunk, sizeof chunk);
                if (got <= 0) {
                    if (got < 0 && errno == EINTR)
                        continue;
                    break;
                }
                buf_add(&raw, chunk, (size_t)got, RAW_CAP);

                if (!handed && wants_screen(chunk, (size_t)got)) {
                    hand_over(was_raw);
                    handed = 1;
                    write_all(STDOUT_FILENO, raw.data, raw.len);
                } else if (handed) {
                    write_all(STDOUT_FILENO, chunk, (size_t)got);
                } else if (now_seconds() - shown >= 0.09) {
                    shown = now_seconds();
                    char *live = plain_text(raw.data ? raw.data : "", raw.len);
                    if (live) {
                        ran_set(mark, live);
                        free(live);
                    }
                }
            }
            if (stdin_open && FD_ISSET(STDIN_FILENO, &fds)) {
                ssize_t got = read(STDIN_FILENO, chunk, sizeof chunk);
                if (got > 0)
                    write_all(master, chunk, (size_t)got);
                else if (got == 0 || errno != EINTR)
                    stdin_open = 0;
            }
        }
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
    }
    if (master >= 0)
        close(master);

    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGQUIT, &old_quit, NULL);

    if (handed && mark) {
        if (was_raw) {
            ui_esc("\x1b[?2004h");
            ui_raw(1);
            ui_cursor_plain();
        }
        block_forget();
        viewport_resume();
    }

    if (pid < 0) {
        ui_error("could not run the shell");
    } else {
        char *text = plain_text(raw.data ? raw.data : "", raw.len);
        if (text) {
            if (raw.full) {
                const char *note = "\n… output truncated …";
                char       *more = realloc(text, strlen(text) + strlen(note) + 1);
                if (more) {
                    text = more;
                    strcat(text, note);
                }
            }
            text = elide(text);
            // The same text the model is given: what the command said, cut to
            // a size worth keeping. A command that took the screen is not
            // written down — a whole editing session replayed as text is not
            // a record of anything.
            if (!handed)
                ran_set(mark, text);
            context_add(cmd, text, status);
            free(text);
        }
        if (WIFSIGNALED(status))
            ui_error("terminated by signal %d", WTERMSIG(status));
        else if (WIFEXITED(status) && WEXITSTATUS(status))
            ui_error("exit %d", WEXITSTATUS(status));
    }
    free(raw.data);

    ui_put("\n");
    ui_flush();
}
