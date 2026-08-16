#include "tty.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#define BRACKETED_PASTE_ON  "\x1b[?2004h"
#define BRACKETED_PASTE_OFF "\x1b[?2004l"

#define ESC_GRACE_MS 30

static struct termios entry_mode;
static int in_raw;
static volatile sig_atomic_t got_winch;

static unsigned char pending[4096];
static size_t pending_len, pending_pos;

static volatile sig_atomic_t winch_count;

static void on_winch(int sig) { (void)sig; got_winch = 1; winch_count++; }

/* atexit() does not run when a signal kills us, which would leave the user's
   terminal in raw mode with echo off. Only async-signal-safe calls here. */
static void on_fatal(int sig)
{
    if (in_raw) {
        (void)!write(STDOUT_FILENO, BRACKETED_PASTE_OFF "\x1b[?25h",
                     sizeof(BRACKETED_PASTE_OFF "\x1b[?25h") - 1);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &entry_mode);
        in_raw = 0;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

int tty_is_raw(void) { return in_raw; }

int tty_cooked_termios(struct termios *out)
{
    if (in_raw) {
        *out = entry_mode;
        return 0;
    }
    return tcgetattr(STDIN_FILENO, out) == 0 ? 0 : -1;
}

size_t tty_take_pending(void *buf, size_t max)
{
    size_t have = pending_len - pending_pos;
    if (have > max)
        have = max;
    memcpy(buf, pending + pending_pos, have);
    pending_pos += have;
    return have;
}

unsigned tty_resize_epoch(void) { return (unsigned)winch_count; }

int tty_columns(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col < 20 ? 20 : ws.ws_col;
    return 80;
}

int tty_rows(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return ws.ws_row < 3 ? 3 : ws.ws_row;
    return 24;
}

int tty_raw_begin(void)
{
    if (in_raw)
        return 0;
    if (!isatty(STDIN_FILENO))
        return -1;
    if (tcgetattr(STDIN_FILENO, &entry_mode) != 0)
        return -1;

    struct termios raw = entry_mode;
    raw.c_iflag &= ~(unsigned long)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(unsigned long)(OPOST);
    raw.c_lflag &= ~(unsigned long)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0)
        return -1;

    in_raw = 1;
    struct sigaction sa = {0};
    sa.sa_handler = on_winch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);

    static const int FATAL[] = {SIGHUP, SIGINT, SIGTERM, SIGQUIT, SIGSEGV, SIGBUS, SIGABRT};
    struct sigaction fs = {0};
    fs.sa_handler = on_fatal;
    sigemptyset(&fs.sa_mask);
    for (size_t i = 0; i < sizeof FATAL / sizeof *FATAL; i++)
        sigaction(FATAL[i], &fs, NULL);

    signal(SIGPIPE, SIG_IGN);

    fputs(BRACKETED_PASTE_ON, stdout);
    fflush(stdout);
    return 0;
}

void tty_raw_end(void)
{
    if (!in_raw)
        return;
    fputs(BRACKETED_PASTE_OFF "\x1b[?25h", stdout);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &entry_mode);
    in_raw = 0;
}

static int  (*watch_fd)(void *ud);
static void (*watch_ready)(void *ud);
static void *watch_ud;

void tty_watch(int (*fd)(void *ud), void (*ready)(void *ud), void *ud)
{
    watch_fd = fd;
    watch_ready = ready;
    watch_ud = ud;
}

static int wait_readable(int timeout_ms)
{
    for (;;) {
        int extra = watch_fd ? watch_fd(watch_ud) : -1;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        int last = STDIN_FILENO;
        if (extra >= 0) {
            FD_SET(extra, &fds);
            if (extra > last)
                last = extra;
        }
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        int r = select(last + 1, &fds, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
        if (r < 0)
            return errno == EINTR ? 0 : -1;
        if (r == 0)
            return 0;
        if (FD_ISSET(STDIN_FILENO, &fds))
            return 1;
        if (extra < 0 || !FD_ISSET(extra, &fds))
            return 0;
        watch_ready(watch_ud);

        if (timeout_ms >= 0)
            return 0;
    }
}

static int refill(int timeout_ms)
{
    if (pending_pos < pending_len)
        return (int)(pending_len - pending_pos);
    pending_pos = pending_len = 0;

    int ready = wait_readable(timeout_ms);
    if (ready <= 0)
        return ready < 0 ? -1 : 0;

    ssize_t n = read(STDIN_FILENO, pending, sizeof pending);
    if (n <= 0)
        return (n == 0 || errno != EINTR) ? -1 : 0;
    pending_len = (size_t)n;
    return (int)pending_len;
}

static int peek_byte(int timeout_ms)
{
    if (pending_pos >= pending_len && refill(timeout_ms) <= 0)
        return -1;
    return pending[pending_pos];
}

static int take_byte(int timeout_ms)
{
    int b = peek_byte(timeout_ms);
    if (b >= 0)
        pending_pos++;
    return b;
}

static void emit(tty_event *ev, tty_key key)
{
    ev->key = key;
    ev->cp = 0;
    ev->text = NULL;
}

static void read_paste(tty_event *ev)
{
    size_t cap = 256, len = 0;
    char *body = malloc(cap);
    if (!body) {
        emit(ev, TK_ESCAPE);
        return;
    }
    static const char END[] = "\x1b[201~";
    /* Keep consuming to the end marker once the cap is hit, or the rest of the
       paste would be re-read as keystrokes. */
    const size_t PASTE_MAX = 8u << 20;
    int full = 0;
    size_t matched = 0;
    for (;;) {
        int b = take_byte(2000);
        if (b < 0)
            break;
        if ((char)b == END[matched]) {
            if (++matched == sizeof END - 1)
                break;
            continue;
        }
        if (len >= PASTE_MAX)
            full = 1;
        if (full) {
            matched = 0;
            continue;
        }

        for (size_t i = 0; i < matched; i++) {
            if (len + 2 > cap) {
                char *g = realloc(body, cap * 2);
                if (!g)
                    goto done;
                body = g;
                cap *= 2;
            }
            body[len++] = END[i];
        }
        matched = 0;
        if (len + 2 > cap) {
            char *g = realloc(body, cap * 2);
            if (!g)
                break;
            body = g;
            cap *= 2;
        }
        body[len++] = (char)b;
    }
done:
    body[len] = '\0';
    ev->key = TK_TEXT;
    ev->cp = 0;
    ev->text = body;
}

static void decode_csi(tty_event *ev, const int *params, int nparams, int final)
{
    int mods = nparams >= 2 ? params[1] : 1;
    int ctrl_or_alt = (mods == 5 || mods == 3 || mods == 7 || mods == 9);

    switch (final) {
    case 'A': emit(ev, TK_UP); return;
    case 'B': emit(ev, TK_DOWN); return;
    case 'C': emit(ev, ctrl_or_alt ? TK_WORD_RIGHT : TK_RIGHT); return;
    case 'D': emit(ev, ctrl_or_alt ? TK_WORD_LEFT : TK_LEFT); return;
    case 'H': emit(ev, TK_HOME); return;
    case 'F': emit(ev, TK_END); return;
    case 'u':

        if (nparams >= 1 && params[0] == 13) {
            emit(ev, mods > 1 ? TK_NEWLINE : TK_ENTER);
            return;
        }
        if (nparams >= 1 && params[0] >= 32) {
            ev->key = TK_CHAR;
            ev->cp = (uint32_t)params[0];
            ev->text = NULL;
            return;
        }
        emit(ev, TK_ESCAPE);
        return;
    case '~':
        switch (nparams >= 1 ? params[0] : 0) {
        case 1: case 7: emit(ev, TK_HOME); return;
        case 3:         emit(ev, TK_DELETE); return;
        case 4: case 8: emit(ev, TK_END); return;
        default:        emit(ev, TK_ESCAPE); return;
        }
    default:
        emit(ev, TK_ESCAPE);
        return;
    }
}

static void decode_escape(tty_event *ev)
{
    int b = peek_byte(ESC_GRACE_MS);
    if (b < 0) {
        emit(ev, TK_ESCAPE);
        return;
    }

    if (b == '[') {
        pending_pos++;
        int params[8] = {0};
        int nparams = 0, have_digits = 0;
        for (;;) {
            int c = take_byte(50);
            if (c < 0) {
                emit(ev, TK_ESCAPE);
                return;
            }
            if (c >= '0' && c <= '9') {
                if (nparams < 8) {
                    params[nparams] = params[nparams] * 10 + (c - '0');
                    have_digits = 1;
                }
                continue;
            }
            if (c == ';' || c == ':') {
                if (nparams < 7)
                    nparams++;
                have_digits = 1;
                continue;
            }
            if (c == '?' || c == '<' || c == '>')
                continue;
            if (have_digits)
                nparams++;
            if (c == '~' && nparams >= 1 && params[0] == 200) {
                read_paste(ev);
                return;
            }
            decode_csi(ev, params, nparams, c);
            return;
        }
    }

    if (b == 'O') {
        pending_pos++;
        int c = take_byte(50);
        switch (c) {
        case 'A': emit(ev, TK_UP); return;
        case 'B': emit(ev, TK_DOWN); return;
        case 'C': emit(ev, TK_RIGHT); return;
        case 'D': emit(ev, TK_LEFT); return;
        case 'H': emit(ev, TK_HOME); return;
        case 'F': emit(ev, TK_END); return;
        default:  emit(ev, TK_ESCAPE); return;
        }
    }

    pending_pos++;
    switch (b) {
    case 'b': emit(ev, TK_WORD_LEFT); return;
    case 'f': emit(ev, TK_WORD_RIGHT); return;
    case '\r': case '\n': emit(ev, TK_NEWLINE); return;
    case 0x7f: emit(ev, TK_WORD_LEFT); return;
    default: emit(ev, TK_ESCAPE); return;
    }
}

static uint32_t decode_utf8(int lead)
{
    int extra;
    uint32_t cp;
    if (lead < 0xC0)      return '?';
    else if (lead < 0xE0) { cp = (uint32_t)lead & 0x1F; extra = 1; }
    else if (lead < 0xF0) { cp = (uint32_t)lead & 0x0F; extra = 2; }
    else                  { cp = (uint32_t)lead & 0x07; extra = 3; }

    for (int i = 0; i < extra; i++) {
        int c = peek_byte(50);
        if (c < 0 || (c & 0xC0) != 0x80)
            return '?';
        pending_pos++;
        cp = (cp << 6) | ((uint32_t)c & 0x3F);
    }
    return cp;
}

int tty_read(tty_event *ev, int timeout_ms)
{
    if (got_winch) {
        got_winch = 0;
        emit(ev, TK_RESIZE);
        return 1;
    }

    int avail = refill(timeout_ms);
    if (avail == 0) {
        if (got_winch) {
            got_winch = 0;
            emit(ev, TK_RESIZE);
            return 1;
        }
        return 0;
    }
    if (avail < 0) {
        emit(ev, TK_EOF);
        return 1;
    }

    int b = pending[pending_pos++];
    switch (b) {
    case 0x1b: decode_escape(ev); return 1;
    case '\r': emit(ev, TK_ENTER); return 1;
    case '\n': emit(ev, TK_NEWLINE); return 1;
    case '\t': emit(ev, TK_TAB); return 1;
    case 0x7f:
    case 0x08: emit(ev, TK_BACKSPACE); return 1;
    default: break;
    }

    ev->key = TK_CHAR;
    ev->text = NULL;
    ev->cp = (b < 0x80) ? (uint32_t)b : decode_utf8(b);
    return 1;
}
