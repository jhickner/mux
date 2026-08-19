#include "viewport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tty.h"
#include "ui.h"

// Rows kept before the oldest are dropped. Generous enough that a long session
// keeps everything worth scrolling back to.
#define ROWS_KEEP 20000

static char **rows;
static int    nrows, rows_cap;

// The row still being written: output arrives mid-row and only becomes a row
// of its own at a newline.
static char  *open_row;
static size_t open_len, open_cap;

static char **chrome_rows;
static int    chrome_n, chrome_cap;
static int    chrome_caret_row, chrome_caret_col = -1;

static int active;
static int suspended;
static int scrolled;
static int dirty;

static void direct(const char *s, size_t n)
{
    fwrite(s, 1, n, stdout);
}

static void direct_str(const char *s) { direct(s, strlen(s)); }

static void cup(int row, int col)
{
    char esc[32];
    snprintf(esc, sizeof esc, "\x1b[%d;%dH", row, col);
    direct_str(esc);
}

// While suspended the screen belongs to something else — a child process, or
// a full-screen widget of our own — so output goes straight to the terminal
// instead of into the transcript.
int viewport_active(void) { return active && !suspended; }

void viewport_touch(void) { dirty = 1; }

int viewport_rows(void) { return nrows + (open_len > 0 ? 1 : 0); }

int viewport_scrolled(void) { return scrolled; }

/* --- the row store ------------------------------------------------------- */

static void rows_push(char *s)
{
    if (nrows == rows_cap) {
        int cap = rows_cap ? rows_cap * 2 : 256;
        char **grown = realloc(rows, (size_t)cap * sizeof *grown);
        if (!grown) {
            free(s);
            return;
        }
        rows = grown;
        rows_cap = cap;
    }
    rows[nrows++] = s;

    if (nrows > ROWS_KEEP) {
        int drop = nrows - ROWS_KEEP;
        for (int i = 0; i < drop; i++)
            free(rows[i]);
        memmove(rows, rows + drop, (size_t)(nrows - drop) * sizeof *rows);
        nrows -= drop;
    }
}

static void open_close(void)
{
    char *s = open_len ? strndup(open_row, open_len) : strdup("");
    open_len = 0;
    if (s)
        rows_push(s);
}

static void open_append(const char *s, size_t n)
{
    if (open_len + n + 1 > open_cap) {
        size_t cap = open_cap ? open_cap : 256;
        while (cap < open_len + n + 1)
            cap *= 2;
        char *grown = realloc(open_row, cap);
        if (!grown)
            return;
        open_row = grown;
        open_cap = cap;
    }
    memcpy(open_row + open_len, s, n);
    open_len += n;
    open_row[open_len] = '\0';
}

void viewport_write(const char *s, size_t n)
{
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') {
            open_append(s + start, i - start);
            open_close();
            start = i + 1;
        } else if (s[i] == '\r') {
            // A carriage return rewrites the row from its start.
            open_append(s + start, i - start);
            open_len = 0;
            start = i + 1;
        }
    }
    open_append(s + start, n - start);

    // New output means following the tail again.
    scrolled = 0;
    dirty = 1;
}

void viewport_drop_row(void)
{
    if (open_len) {
        open_len = 0;
        dirty = 1;
        return;
    }
    if (nrows > 0) {
        free(rows[--nrows]);
        dirty = 1;
    }
}

void viewport_clear(void)
{
    for (int i = 0; i < nrows; i++)
        free(rows[i]);
    nrows = 0;
    open_len = 0;
    scrolled = 0;
    dirty = 1;
}

/* --- style carried across a soft wrap ------------------------------------ */

// A row wider than the screen is painted over several screen rows, and the
// continuation has to start in the style the cut left behind. mux emits one
// complete SGR per role and a reset between them, so everything since the last
// reset is the state.
struct style {
    char buf[512];
    size_t len;
};

static void style_add(struct style *st, const char *s, size_t n)
{
    if (st->len + n >= sizeof st->buf)
        return;
    memcpy(st->buf + st->len, s, n);
    st->len += n;
    st->buf[st->len] = '\0';
}

static int sgr_is_reset(const char *s, size_t n)
{
    // ESC [ ... m, with no parameters or a single zero.
    size_t i = 2;
    if (i >= n)
        return 1;
    if (s[i] == 'm')
        return 1;
    for (; i < n && s[i] != 'm'; i++)
        if (s[i] != '0')
            return 0;
    return 1;
}

// Consumes one escape sequence or one codepoint at `i`, adding its cells and
// folding any style it carries into `st`.
static size_t step(const char *s, size_t n, size_t i, size_t *cells, struct style *st)
{
    if (s[i] != '\x1b') {
        size_t start = i++;
        while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80)
            i++;
        *cells += ui_cells_visible(s + start, i - start);
        return i;
    }

    size_t j = i + 1;
    if (j < n && s[j] == '[') {
        for (j++; j < n && (s[j] < '@' || s[j] > '~'); j++)
            ;
        size_t end = j < n ? j + 1 : n;
        if (st && j < n && s[j] == 'm') {
            if (sgr_is_reset(s + i, end - i))
                st->len = 0, st->buf[0] = '\0';
            else
                style_add(st, s + i, end - i);
        }
        return end;
    }
    if (j < n && (s[j] == ']' || s[j] == 'P' || s[j] == '_' || s[j] == '^' || s[j] == 'X')) {
        int osc8 = j + 1 < n && s[j] == ']' && s[j + 1] == '8';
        for (j++; j < n && s[j] != '\a' && s[j] != '\x1b'; j++)
            ;
        if (j < n && s[j] == '\x1b')
            j++;
        size_t end = j < n ? j + 1 : n;
        // A hyperlink spans the wrap, so it has to be reopened with the style.
        if (st && osc8)
            style_add(st, s + i, end - i);
        return end;
    }
    return j < n ? j + 1 : n;
}

// How many screen rows `s` needs at width W.
static int wrap_count(const char *s, int W)
{
    size_t n = strlen(s);
    if (n == 0 || W < 1)
        return 1;
    int    used = 1;
    size_t cells = 0, i = 0;
    while (i < n) {
        size_t was = cells;
        i = step(s, n, i, &cells, NULL);
        if (cells > (size_t)W && was > 0) {
            used++;
            cells -= was;
        }
    }
    return used;
}

// Paints `s` across the screen rows starting at `top`, at most `limit` of them,
// and returns how many it used.
static int paint_row(const char *s, int top, int limit, int W)
{
    size_t n = strlen(s);
    struct style st = {{0}, 0};
    int    used = 0;
    size_t i = 0, cells = 0, start = 0;

    while (used < limit) {
        cup(top + used, 1);
        direct_str("\x1b[0m\x1b[K");
        if (st.len)
            direct(st.buf, st.len);

        size_t line_start = start;
        cells = 0;
        while (i < n) {
            size_t was = cells;
            size_t next = step(s, n, i, &cells, &st);
            if (cells > (size_t)W && was > 0)
                break;
            i = next;
        }
        direct(s + line_start, i - line_start);
        used++;
        start = i;
        if (i >= n)
            break;
    }
    return used ? used : 1;
}

/* --- painting ------------------------------------------------------------ */

void viewport_paint(void)
{
    if (!active || suspended)
        return;

    int H = tty_rows(), W = tty_screen_columns();
    if (H < 1 || W < 1)
        return;

    int ch = chrome_n;
    if (ch > H - 1)
        ch = H - 1;
    if (ch < 0)
        ch = 0;
    int body = H - ch;

    // The open row shows as a row of its own until its newline arrives.
    int total = nrows;
    char *tail = NULL;
    if (open_len) {
        tail = open_row;
        total++;
    }

    // Walk back from the newest row until the window is covered, so the cost
    // is the size of the window rather than the size of the history.
    int want = body + scrolled;
    int first = total;
    int have = 0;
    while (first > 0 && have < want) {
        const char *s = (first - 1 == nrows && tail) ? tail : rows[first - 1];
        have += wrap_count(s, W);
        first--;
    }
    if (scrolled > have - body)
        scrolled = have - body > 0 ? have - body : 0;

    // Rows of the first stored row that fall above the window.
    int skip = have - body - scrolled;
    if (skip < 0)
        skip = 0;

    // A transcript shorter than the window sits on the bottom of it, against
    // the chrome, rather than hanging from the top of the screen.
    int fill = have - skip;
    int pad = body - fill;
    if (pad < 0)
        pad = 0;

    direct_str("\x1b[?2026h");
    direct_str("\x1b[?25l");
    direct_str("\x1b[?7l");

    int at = 1;
    for (; at <= pad; at++) {
        cup(at, 1);
        direct_str("\x1b[0m\x1b[K");
    }

    for (int r = first; r < total && at <= body; r++) {
        const char *s = (r == nrows && tail) ? tail : rows[r];
        int need = wrap_count(s, W);
        if (skip >= need) {
            skip -= need;
            continue;
        }
        // Painting from the top of a row that is partly above the window would
        // need a mid-row start; the window is sized so this only trims the
        // oldest row, and starting it whole is close enough to keep simple.
        skip = 0;
        at += paint_row(s, at, body - at + 1, W);
    }
    for (; at <= body; at++) {
        cup(at, 1);
        direct_str("\x1b[0m\x1b[K");
    }

    for (int i = 0; i < ch; i++) {
        cup(body + 1 + i, 1);
        direct_str("\x1b[0m\x1b[K");
        direct_str(chrome_rows[i]);
    }

    direct_str("\x1b[?7h");
    direct_str("\x1b[0m");

    if (chrome_caret_col >= 0 && chrome_caret_row >= 0 && chrome_caret_row < ch) {
        int col = chrome_caret_col + 1;
        if (col > W)
            col = W;
        cup(body + 1 + chrome_caret_row, col);
        direct_str("\x1b[?25h");
    }

    direct_str("\x1b[?2026l");
    fflush(stdout);
    dirty = 0;
}

int viewport_first_visible(void)
{
    int H = tty_rows();
    int ch = chrome_n > H - 1 ? H - 1 : chrome_n;
    int body = H - (ch > 0 ? ch : 0);
    int first = viewport_rows() - body - scrolled;
    return first > 0 ? first : 0;
}

/* --- chrome -------------------------------------------------------------- */

void viewport_chrome(char **rows_in, int n, int caret_row, int caret_col)
{
    for (int i = 0; i < chrome_n; i++)
        free(chrome_rows[i]);
    chrome_n = 0;

    if (n > chrome_cap) {
        char **grown = realloc(chrome_rows, (size_t)n * sizeof *grown);
        if (!grown)
            return;
        chrome_rows = grown;
        chrome_cap = n;
    }
    for (int i = 0; i < n; i++) {
        chrome_rows[i] = strdup(rows_in[i] ? rows_in[i] : "");
        if (!chrome_rows[i])
            break;
        chrome_n++;
    }
    chrome_caret_row = caret_row;
    chrome_caret_col = caret_col;
    dirty = 1;
}

void viewport_chrome_row(int at, const char *s)
{
    if (at < 0 || at >= chrome_n || !s)
        return;
    char *copy = strdup(s);
    if (!copy)
        return;
    free(chrome_rows[at]);
    chrome_rows[at] = copy;
    dirty = 1;
}

void viewport_chrome_keep(int keep)
{
    if (keep > chrome_n)
        keep = chrome_n;
    for (int i = 0; i < keep; i++) {
        rows_push(chrome_rows[i]);
        chrome_rows[i] = NULL;
    }
    for (int i = keep; i < chrome_n; i++)
        free(chrome_rows[i]);
    chrome_n = 0;
    chrome_caret_col = -1;
    scrolled = 0;
    dirty = 1;
}

void viewport_chrome_clear(void)
{
    for (int i = 0; i < chrome_n; i++)
        free(chrome_rows[i]);
    chrome_n = 0;
    chrome_caret_col = -1;
    dirty = 1;
}

/* --- scrolling ----------------------------------------------------------- */

void viewport_scroll(int delta)
{
    scrolled += delta;
    if (scrolled < 0)
        scrolled = 0;
    dirty = 1;
    viewport_paint();
}

void viewport_scroll_end(void)
{
    scrolled = 0;
    dirty = 1;
    viewport_paint();
}

/* --- the screen ---------------------------------------------------------- */

// Mouse reporting stays off until tty.c can decode the events: enabling it
// without a parser turns every wheel tick into junk in the prompt.
void viewport_begin(void)
{
    if (active)
        return;
    active = 1;
    direct_str("\x1b[?1049h");
    fflush(stdout);
    viewport_paint();
}

void viewport_end(void)
{
    if (!active)
        return;
    active = 0;
    suspended = 0;
    direct_str("\x1b[?25h");
    direct_str("\x1b[?1049l");
    fflush(stdout);
}

void viewport_dump(void)
{
    for (int i = 0; i < nrows; i++) {
        direct_str(rows[i]);
        direct_str("\x1b[0m\r\n");
    }
    if (open_len) {
        direct(open_row, open_len);
        direct_str("\x1b[0m\r\n");
    }
    fflush(stdout);
}

void viewport_suspend(void)
{
    if (!active || suspended)
        return;
    suspended = 1;
    direct_str("\x1b[?25h");
    direct_str("\x1b[?1049l");
    fflush(stdout);
}

void viewport_resume(void)
{
    if (!active || !suspended)
        return;
    suspended = 0;
    direct_str("\x1b[?1049h");
    fflush(stdout);
    viewport_paint();
}
