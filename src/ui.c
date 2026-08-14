#include "ui.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>

#include "tty.h"
#include "vendor/screen_color.h"
#include "vendor/colors.h"

static int use_color;
static int raw_newlines;

static struct {
    int enabled;
    int tracking;
    int pinned;
    int prompt_top;
    int prompt_rows;
    int cursor_row;
    int cursor_col;
    int screen_rows;
    int screen_cols;
    int hidden;
    int tmux_hook;
    int tmux_hook_index;
    char *text;
} sticky;

static volatile sig_atomic_t sticky_mode_changed;
static struct sigaction sticky_old_sigusr2;

static void sticky_activate(void);
static void sticky_after_linefeed(void);
static void sticky_note_escape(const char *s);
static void sticky_tmux_refresh(void);

/* Each role is an optional SGR attribute plus a foreground drawn from the
 * active colors.h theme. `slot` is -1 for the attribute-only roles. */
static const struct {
    const char *attr;
    int         slot;
} ROLES[UI_RESET] = {
    [UI_ACCENT]  = { NULL, COLOR_BASE12 },
    [UI_TEXT]    = { NULL, COLOR_UI_TYPED },
    [UI_CHROME]  = { NULL, COLOR_UI_BORDER_FLOAT },
    [UI_DIM]     = { NULL, COLOR_UI_DIM },
    [UI_BODY]    = { NULL, COLOR_BASE5 },
    [UI_BOLD]    = { "1",  COLOR_BASE6 },
    [UI_ITALIC]  = { "3",  COLOR_BASE6 },
    [UI_CODE]    = { NULL, COLOR_BASE6 },
    [UI_HEADING] = { "1",  COLOR_BASE7 },
    [UI_LINK]    = { "4",  COLOR_BASE13 },
    [UI_ERROR]   = { NULL, COLOR_UI_MSG_ERROR },
    [UI_OK]      = { NULL, COLOR_BASE11 },
    [UI_THINKING] = { "3", COLOR_BASE14 },
    [UI_TOOL]    = { NULL, COLOR_BASE10 },
    [UI_TOOLDIM] = { "2",  COLOR_BASE10 },
};

static char styles[UI_RESET][32];

void ui_init(void)
{
    const char *no_color = getenv("NO_COLOR");
    use_color = isatty(STDOUT_FILENO) && !(no_color && *no_color);
    if (!use_color)
        return;

    colors_init();
    for (int i = 0; i < UI_RESET; i++) {
        const char *attr = ROLES[i].attr;
        if (ROLES[i].slot < 0) {
            snprintf(styles[i], sizeof styles[i], "\x1b[%sm", attr ? attr : "0");
            continue;
        }
        Color c = color_get((ColorIndex)ROLES[i].slot);
        snprintf(styles[i], sizeof styles[i], "\x1b[%s%s38;2;%u;%u;%um",
                 attr ? attr : "", attr ? ";" : "", c.r, c.g, c.b);
    }
}

const char *ui_style(enum ui_role role)
{
    if (!use_color)
        return "";
    if (role == UI_RESET)
        return "\x1b[0m";
    if (role < 0 || role >= UI_RESET)
        return "";
    return styles[role];
}

void ui_cursor_plain(void)
{
    if (!use_color)
        return;
    Color c = color_get(COLOR_UI_CURSOR_FG);
    printf("\x1b]12;#%02x%02x%02x\x07", c.r, c.g, c.b);
    fflush(stdout);
}

void ui_cursor_restore(void)
{
    if (!use_color)
        return;
    fputs("\x1b]112\x07", stdout);
    fflush(stdout);
}

void ui_raw(int on) { raw_newlines = on; }

void ui_putn(const char *s, size_t n)
{
    if (!raw_newlines) {
        fwrite(s, 1, n, stdout);
        return;
    }
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\n')
            continue;
        fwrite(s + start, 1, i - start, stdout);
        fputs("\r\n", stdout);
        sticky.cursor_col = 1;
        sticky_after_linefeed();
        start = i + 1;
    }
    if (start < n)
        fwrite(s + start, 1, n - start, stdout);
}

void ui_put(const char *s)
{
    if (s)
        ui_putn(s, strlen(s));
}

void ui_printf(const char *fmt, ...)
{
    char stack[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stack, sizeof stack, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof stack) {
        ui_putn(stack, (size_t)n);
        return;
    }
    char *heap = malloc((size_t)n + 1);
    if (!heap)
        return;
    va_start(ap, fmt);
    vsnprintf(heap, (size_t)n + 1, fmt, ap);
    va_end(ap);
    ui_putn(heap, (size_t)n);
    free(heap);
}

void ui_esc(const char *s)
{
    fputs(s, stdout);
    sticky_note_escape(s);
}

void ui_sync_begin(void) { fputs("\x1b[?2026h", stdout); }
void ui_sync_end(void) { fputs("\x1b[?2026l", stdout); }

static int sticky_message_rows(const char *text, int cols)
{
    size_t budget = (size_t)(cols - 2 > 4 ? cols - 2 : 4);
    int rows = 0, first = 1;
    const char *p = text ? text : "";
    while (*p || first) {
        size_t skip = 0;
        size_t row = *p ? ui_wrap_row(p, budget, &skip) : 0;
        rows++;
        p += row + skip;
        first = 0;
    }
    return rows;
}

/* DECSTBM moves the cursor while changing the scrolling margins. Put it back
 * explicitly so callers can turn the feature on and off between ordinary
 * writes without otherwise changing the terminal's output position. */
static void sticky_margin(int top, int bottom, int row, int column)
{
    char esc[64];
    if (top > 0)
        snprintf(esc, sizeof esc, "\x1b[%d;%dr\x1b[%d;%dH",
                 top, bottom, row, column);
    else
        snprintf(esc, sizeof esc, "\x1b[r\x1b[%d;%dH", row, column);
    fputs(esc, stdout); /* deliberately bypass ui_esc's cursor bookkeeping */
    fflush(stdout);
}

/* Run one tmux command without involving a shell. These calls happen only
 * when sticky mode is toggled or copy mode changes, never in the paint loop. */
static int sticky_tmux(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        FILE *null = fopen("/dev/null", "w");
        if (null) {
            dup2(fileno(null), STDOUT_FILENO);
            dup2(fileno(null), STDERR_FILENO);
        }
        execvp("tmux", argv);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return 0;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void sticky_tmux_signal(int sig)
{
    (void)sig;
    sticky_mode_changed = 1;
}

static void sticky_tmux_hook(int on)
{
    const char *pane = getenv("TMUX_PANE");
    if (!pane || !*pane)
        return;

    char hook[64];
    if (!sticky.tmux_hook_index)
        sticky.tmux_hook_index = (int)getpid();
    snprintf(hook, sizeof hook, "pane-mode-changed[%d]",
             sticky.tmux_hook_index);

    if (!on) {
        if (!sticky.tmux_hook)
            return;
        char *const argv[] = {"tmux", "set-hook", "-p", "-u", "-t",
                              (char *)pane, hook, NULL};
        (void)sticky_tmux(argv);
        sigaction(SIGUSR2, &sticky_old_sigusr2, NULL);
        sticky.tmux_hook = 0;
        sticky_mode_changed = 0;
        return;
    }
    if (sticky.tmux_hook)
        return;

    struct sigaction sa = {0};
    sa.sa_handler = sticky_tmux_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR2, &sa, &sticky_old_sigusr2) != 0)
        return;

    char command[96];
    snprintf(command, sizeof command, "run-shell 'kill -USR2 %ld'",
             (long)getpid());
    char *const argv[] = {"tmux", "set-hook", "-p", "-t", (char *)pane,
                          hook, command, NULL};
    if (!sticky_tmux(argv)) {
        sigaction(SIGUSR2, &sticky_old_sigusr2, NULL);
        return;
    }
    sticky.tmux_hook = 1;
}

/* Ask whether this pane is in copy mode. A pipe is used instead of shell
 * interpolation so unusual socket and pane names remain harmless. */
static int sticky_tmux_in_mode(void)
{
    const char *pane = getenv("TMUX_PANE");
    if (!pane || !*pane)
        return 0;
    int fds[2];
    if (pipe(fds) != 0)
        return 0;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execlp("tmux", "tmux", "display-message", "-p", "-t", pane,
               "#{pane_in_mode}", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    char answer = '0';
    ssize_t n;
    do n = read(fds[0], &answer, 1); while (n < 0 && errno == EINTR);
    close(fds[0]);
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return n == 1 && answer != '0' && answer != '\n';
}

static void sticky_paint(int show)
{
    int row = sticky.cursor_row, column = sticky.cursor_col;
    (void)tty_cursor_position(&row, &column);

    fputs("\x1b[?2026h", stdout);
    const char *p = sticky.text ? sticky.text : "";
    size_t budget = (size_t)(sticky.screen_cols - 2 > 4
                                 ? sticky.screen_cols - 2 : 4);
    for (int screen_row = 1; screen_row <= sticky.prompt_rows; screen_row++) {
        fprintf(stdout, "\x1b[%d;1H\x1b[2K", screen_row);
        if (show) {
            size_t skip = 0;
            size_t bytes = *p ? ui_wrap_row(p, budget, &skip) : 0;
            fputs(ui_style(UI_ACCENT), stdout);
            fputs(UI_BAR " ", stdout);
            fwrite(p, 1, bytes, stdout);
            fputs(ui_style(UI_RESET), stdout);
            p += bytes + skip;
        }
    }
    fprintf(stdout, "\x1b[%d;%dH\x1b[?2026l", row, column);
    fflush(stdout);
    sticky.cursor_row = row;
    sticky.cursor_col = column;
}

static void sticky_tmux_refresh(void)
{
    const char *pane = getenv("TMUX_PANE");
    if (!pane || !*pane)
        return;
    char *const argv[] = {"tmux", "send-keys", "-t", (char *)pane,
                          "-X", "refresh-from-pane", NULL};
    (void)sticky_tmux(argv);
}

void ui_sticky_sync(void)
{
    if (!sticky_mode_changed || !sticky.tmux_hook)
        return;
    sticky_mode_changed = 0;

    int in_mode = sticky_tmux_in_mode();
    if (!sticky.tracking || !sticky.pinned)
        return;
    if (tty_rows() != sticky.screen_rows || tty_columns() != sticky.screen_cols) {
        ui_sticky_end();
        return;
    }
    if (in_mode && !sticky.hidden) {
        sticky_paint(0);
        sticky.hidden = 1;
        sticky_tmux_refresh();
    } else if (!in_mode && sticky.hidden) {
        sticky_paint(1);
        sticky.hidden = 0;
    }
}

static void sticky_release(void)
{
    if (!sticky.pinned)
        return;
    int row = sticky.cursor_row, column = sticky.cursor_col;
    (void)tty_cursor_position(&row, &column);
    sticky_margin(0, 0, row, column);
    sticky.cursor_row = row;
    sticky.cursor_col = column;
    sticky.pinned = 0;
}

void ui_sticky_end(void)
{
    sticky_release();
    sticky.tracking = 0;
    sticky.hidden = 0;
    free(sticky.text);
    sticky.text = NULL;
}

void ui_sticky_set(int on)
{
    on = on ? 1 : 0;
    if (!on)
        ui_sticky_end();
    sticky.enabled = on;
    sticky_tmux_hook(on);
}

int ui_sticky_enabled(void) { return sticky.enabled; }

void ui_sticky_begin(const char *text)
{
    if (!sticky.enabled || !raw_newlines || !tty_is_raw())
        return;

    ui_sticky_end();
    sticky.screen_rows = tty_rows();
    sticky.screen_cols = tty_columns();
    sticky.prompt_rows = sticky_message_rows(text, sticky.screen_cols);
    if (sticky.prompt_rows >= sticky.screen_rows - 1)
        return; /* leave room for at least one content row and the editor */

    int row, column;
    if (!tty_cursor_position(&row, &column))
        return; /* the terminal does not implement a cursor-position report */

    /* prompt_echo_message() leaves one blank separator row after the block. */
    sticky.prompt_top = row - sticky.prompt_rows - 1;
    if (sticky.prompt_top < 1)
        return;
    sticky.cursor_row = row;
    sticky.cursor_col = column;
    sticky.text = strdup(text ? text : "");
    if (!sticky.text)
        return;
    sticky.tracking = 1;
    if (sticky.prompt_top == 1)
        sticky_activate();
}

static void sticky_activate(void)
{
    if (!sticky.tracking || sticky.pinned || sticky.prompt_top != 1)
        return;
    int first_content = sticky.prompt_rows + 1;
    if (first_content >= sticky.screen_rows)
        return;
    sticky_margin(first_content, sticky.screen_rows,
                  sticky.cursor_row, sticky.cursor_col);
    sticky.pinned = 1;
}

static void sticky_after_linefeed(void)
{
    if (!sticky.tracking)
        return;

    if (tty_rows() != sticky.screen_rows || tty_columns() != sticky.screen_cols) {
        /* Reflow changes both the prompt height and its position. Releasing is
         * safer than reserving the wrong rows; the next submitted prompt starts
         * a fresh tracker at the new dimensions. */
        ui_sticky_end();
        return;
    }

    if (sticky.cursor_row < sticky.screen_rows) {
        sticky.cursor_row++;
        return;
    }
    if (sticky.pinned)
        return; /* only the content region scrolled */

    if (sticky.prompt_top > 1)
        sticky.prompt_top--;
    if (sticky.prompt_top == 1)
        sticky_activate();
}

/* Cursor motion is sparse in this application but important: the spinner and
 * live editor repeatedly walk up, erase, and repaint. Follow the vertical CSI
 * operations so a later line feed is judged against the real bottom row. */
static void sticky_note_escape(const char *s)
{
    if (!sticky.tracking)
        return;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\r') {
            sticky.cursor_col = 1;
            continue;
        }
        if ((unsigned char)s[i] != 0x1b || s[i + 1] != '[')
            continue;
        size_t j = i + 2;
        if (s[j] == '?' || s[j] == '>' || s[j] == '<')
            j++;
        int first = 0, second = 0, have_first = 0, have_second = 0;
        while (s[j] >= '0' && s[j] <= '9') {
            first = first * 10 + s[j++] - '0';
            have_first = 1;
        }
        if (s[j] == ';') {
            j++;
            while (s[j] >= '0' && s[j] <= '9') {
                second = second * 10 + s[j++] - '0';
                have_second = 1;
            }
        }
        int n = have_first ? first : 1;
        if (s[j] == 'A') {
            sticky.cursor_row -= n;
            if (sticky.cursor_row < 1) sticky.cursor_row = 1;
        } else if (s[j] == 'B') {
            sticky.cursor_row += n;
            if (sticky.cursor_row > sticky.screen_rows)
                sticky.cursor_row = sticky.screen_rows;
        } else if (s[j] == 'H' || s[j] == 'f') {
            sticky.cursor_row = have_first ? first : 1;
            sticky.cursor_col = have_second ? second : 1;
        }
        i = j;
    }
}

int ui_reflow_rows(const int *row_widths, int count, int cols)
{
    if (cols <= 0)
        return count > 0 ? count : 0;
    int rows = 0;
    for (int i = 0; i < count; i++)
        rows += row_widths[i] > 0 ? (row_widths[i] + cols - 1) / cols : 1;
    return rows;
}

void ui_flush(void) { fflush(stdout); }

int ui_columns(void) { return tty_columns(); }

/* Decode one codepoint from s[*i .. n), advancing past it. */
static unsigned decode(const char *s, size_t n, size_t *i)
{
    unsigned char b = (unsigned char)s[*i];
    unsigned cp;
    int extra;
    if (b < 0x80)      { (*i)++; return b; }
    else if (b < 0xC0) { (*i)++; return '?'; }
    else if (b < 0xE0) { cp = b & 0x1Fu; extra = 1; }
    else if (b < 0xF0) { cp = b & 0x0Fu; extra = 2; }
    else               { cp = b & 0x07u; extra = 3; }
    (*i)++;
    for (int k = 0; k < extra && *i < n; k++, (*i)++) {
        if (((unsigned char)s[*i] & 0xC0) != 0x80)
            return '?';
        cp = (cp << 6) | ((unsigned char)s[*i] & 0x3Fu);
    }
    return cp;
}

static int cell_width(unsigned cp)
{
    if (cp == 0)
        return 0;
    if (cp < 0x20 || cp == 0x7f)
        return 0;
    /* Emoji and other wide pictographs that wcwidth() often misreports as 1. */
    if ((cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) ||
        (cp >= 0x1F000 && cp <= 0x1F2FF))
        return 2;
    int w = wcwidth((wchar_t)cp);
    return w < 0 ? 1 : w;
}

size_t ui_cells_n(const char *s, size_t n)
{
    size_t i = 0, cells = 0;
    while (i < n)
        cells += (size_t)cell_width(decode(s, n, &i));
    return cells;
}

size_t ui_cells(const char *s) { return s ? ui_cells_n(s, strlen(s)) : 0; }

size_t ui_wrap_row(const char *s, size_t budget, size_t *skip)
{
    size_t n = strlen(s);
    *skip = 0;
    if (budget < 1)
        budget = 1;

    size_t i = 0, cells = 0, last_space = 0;
    while (i < n) {
        if (s[i] == '\n') {
            *skip = 1;
            return i;
        }
        size_t start = i;
        size_t w = (size_t)cell_width(decode(s, n, &i));
        if (cells + w > budget) {
            if (last_space > 0) {
                *skip = 1;
                return last_space;
            }
            return start; /* one unbroken word: hard-break it */
        }
        cells += w;
        if (s[start] == ' ')
            last_space = start;
    }
    return n;
}

void ui_wrapped(const char *text, int indent, const char *style)
{
    int columns = ui_columns();
    size_t budget = (size_t)(columns - indent > 1 ? columns - indent : 1);
    const char *p = text;
    int first = 1;

    while (*p || first) {
        size_t skip = 0;
        size_t row = *p ? ui_wrap_row(p, budget, &skip) : 0;
        for (int k = 0; k < indent; k++)
            ui_put(" ");
        if (*style)
            ui_esc(style);
        ui_putn(p, row);
        if (*style)
            ui_esc(ui_style(UI_RESET));
        ui_put("\n");
        p += row + skip;
        first = 0;
    }
}

void ui_bar(const char *style, const char *fmt, ...)
{
    ui_esc(ui_style(UI_CHROME));
    ui_put(UI_BAR);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");
    if (style && *style)
        ui_esc(style);

    char stack[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(stack, sizeof stack, fmt, ap);
    va_end(ap);
    ui_put(stack);

    if (style && *style)
        ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static void ui_line(enum ui_role role, const char *fmt, va_list ap)
{
    char stack[1024];
    vsnprintf(stack, sizeof stack, fmt, ap);
    ui_esc(ui_style(role));
    ui_put(stack);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
    ui_flush();
}

void ui_note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ui_line(UI_DIM, fmt, ap);
    va_end(ap);
}

void ui_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ui_line(UI_ERROR, fmt, ap);
    va_end(ap);
}
