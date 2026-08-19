#include "viewport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tty.h"
#include "ui.h"

// Entries kept before the oldest is dropped. Generous enough that a long
// session keeps everything worth scrolling back to.
#define ITEMS_KEEP 8000

// One thing that was printed. `render` draws it again at a given width; when
// it is NULL the entry is raw output that can only be soft-wrapped, and `cols`
// is 0 to say the cached rows belong to no particular width.
struct item {
    viewport_render_fn render;
    void  *ud;
    void (*free_ud)(void *);
    char **rows;
    int    nrows;
    int    cols;
    unsigned id;
};

static struct item *items;
static int    nitems, items_cap;
static unsigned next_id = 1;

// The entry still being written: output arrives mid-entry and only closes when
// a wrapped item ends, or at the newline of unwrapped output.
static char  *open_buf;
static size_t open_len, open_cap;
static viewport_render_fn open_render;
static void  *open_ud;
static void (*open_free)(void *);
static int    open_wrapped;

static char **chrome_rows;
static int    chrome_n, chrome_cap;
static int    chrome_caret_row, chrome_caret_col = -1;

static int active;
static int suspended;
static int scrolled;
static int dirty;

// Button reporting with SGR coordinates: enough for the wheel, and it leaves
// shift-drag to the terminal so selecting text still works.
#define MOUSE_ON  "\x1b[?1000h\x1b[?1006h"
#define MOUSE_OFF "\x1b[?1000l\x1b[?1006l"

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

int viewport_scrolled(void) { return scrolled; }

unsigned viewport_mark(void) { return next_id; }

/* --- the entry store ----------------------------------------------------- */

static void rows_free(struct item *it)
{
    for (int i = 0; i < it->nrows; i++)
        free(it->rows[i]);
    free(it->rows);
    it->rows = NULL;
    it->nrows = 0;
}

static void item_free(struct item *it)
{
    rows_free(it);
    if (it->free_ud && it->ud)
        it->free_ud(it->ud);
    memset(it, 0, sizeof *it);
}

// Splits painted output into the rows it draws as.
static void rows_set(struct item *it, const char *body, int cols)
{
    rows_free(it);
    it->cols = cols;
    if (!body)
        return;

    int cap = 8, n = 0;
    char **out = malloc((size_t)cap * sizeof *out);
    if (!out)
        return;

    const char *p = body;
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        while (len && p[len - 1] == '\r')
            len--;
        if (n == cap) {
            cap *= 2;
            char **grown = realloc(out, (size_t)cap * sizeof *grown);
            if (!grown)
                break;
            out = grown;
        }
        out[n] = strndup(p, len);
        if (!out[n])
            break;
        n++;
        if (!nl)
            break;
        p = nl + 1;
        if (!*p)
            break;
    }
    it->rows = out;
    it->nrows = n;
}

static struct item *items_push(void)
{
    if (nitems == items_cap) {
        int cap = items_cap ? items_cap * 2 : 256;
        struct item *grown = realloc(items, (size_t)cap * sizeof *grown);
        if (!grown)
            return NULL;
        items = grown;
        items_cap = cap;
    }
    struct item *it = &items[nitems++];
    memset(it, 0, sizeof *it);
    it->id = next_id++;

    if (nitems > ITEMS_KEEP) {
        int drop = nitems - ITEMS_KEEP;
        for (int i = 0; i < drop; i++)
            item_free(&items[i]);
        memmove(items, items + drop, (size_t)(nitems - drop) * sizeof *items);
        nitems -= drop;
        it = &items[nitems - 1];
    }
    return it;
}

static void open_append(const char *s, size_t n)
{
    if (open_len + n + 1 > open_cap) {
        size_t cap = open_cap ? open_cap : 256;
        while (cap < open_len + n + 1)
            cap *= 2;
        char *grown = realloc(open_buf, cap);
        if (!grown)
            return;
        open_buf = grown;
        open_cap = cap;
    }
    memcpy(open_buf + open_len, s, n);
    open_len += n;
    open_buf[open_len] = '\0';
}

// Closes what is open into an entry of its own.
static void open_close(int cols)
{
    struct item *it = items_push();
    if (!it) {
        open_len = 0;
        return;
    }
    it->render = open_render;
    it->ud = open_ud;
    it->free_ud = open_free;
    rows_set(it, open_buf ? open_buf : "", cols);

    open_len = 0;
    open_render = NULL;
    open_ud = NULL;
    open_free = NULL;
}

void viewport_item_begin(viewport_render_fn render, void *ud, void (*free_ud)(void *))
{
    // Unwrapped output already in hand belongs to the entry before this one.
    if (viewport_active() && open_len)
        open_close(0);
    open_render = render;
    open_ud = ud;
    open_free = free_ud;

    // With no viewport the output goes straight to the terminal and there is
    // no entry to keep; the payload is still owned here, and freed at the end.
    open_wrapped = viewport_active();
}

void viewport_item_end(void)
{
    if (!open_wrapped) {
        if (open_free && open_ud)
            open_free(open_ud);
        open_render = NULL;
        open_ud = NULL;
        open_free = NULL;
        return;
    }
    open_wrapped = 0;
    open_close(tty_screen_columns());
    scrolled = 0;
    dirty = 1;
}

void viewport_write(const char *s, size_t n)
{
    // Inside a wrapped entry everything is one entry, newlines and all.
    if (open_wrapped) {
        open_append(s, n);
        return;
    }

    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') {
            open_append(s + start, i - start);
            open_close(0);
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
    if (nitems > 0) {
        item_free(&items[--nitems]);
        dirty = 1;
    }
}

void viewport_clear(void)
{
    for (int i = 0; i < nitems; i++)
        item_free(&items[i]);
    nitems = 0;
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

// The rows an entry draws as at this width, re-rendering it if the width has
// changed since last time. Raw entries have no renderer and keep their rows.
static void item_rows(struct item *it, int W)
{
    if (!it->render || it->cols == W)
        return;
    ui_capture_begin(W);
    it->render(it->ud, W);
    char *painted = ui_capture_end();
    rows_set(it, painted ? painted : "", W);
    free(painted);
}

// Screen rows an entry needs at this width.
static int item_height(struct item *it, int W)
{
    item_rows(it, W);
    if (it->nrows == 0)
        return it->render ? 0 : 1;
    int used = 0;
    for (int i = 0; i < it->nrows; i++)
        used += wrap_count(it->rows[i], W);
    return used;
}

// Unwrapped output that has not reached its newline still shows.
static int window_pending(struct item *pending)
{
    if (open_len && !open_wrapped) {
        pending->rows = &open_buf;
        pending->nrows = 1;
        return 1;
    }
    return 0;
}

// Walk back from the newest entry until the window is covered, so the cost is
// the size of the window rather than the size of the history. Returns the
// index of the first entry shown, and how many screen rows it and everything
// after it need.
static int window_first(int W, int body, struct item *pending, int total, int *have_out)
{
    int want = body + scrolled;
    int first = total;
    int have = 0;
    while (first > 0 && have < want) {
        struct item *it = (first - 1 == nitems) ? pending : &items[first - 1];
        have += item_height(it, W);
        first--;
    }
    *have_out = have;
    return first;
}

static int body_rows(void)
{
    int H = tty_rows();
    int ch = chrome_n;
    if (ch > H - 1)
        ch = H - 1;
    if (ch < 0)
        ch = 0;
    return H - ch;
}

// Asked, not remembered: the window is recomputed here rather than read off
// the last paint, so the answer is right even before the next frame lands.
int viewport_visible(unsigned mark)
{
    int W = tty_screen_columns();
    if (W < 1 || tty_rows() < 1)
        return 1;

    struct item pending = {0};
    int total = nitems + window_pending(&pending);
    int have = 0;
    int first = window_first(W, body_rows(), &pending, total, &have);
    return first >= nitems ? mark >= next_id : mark >= items[first].id;
}

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

    struct item pending = {0};
    int total = nitems + window_pending(&pending);

    int have = 0;
    int first = window_first(W, body, &pending, total, &have);
    if (scrolled > have - body)
        scrolled = have - body > 0 ? have - body : 0;

    // Screen rows of the first entry that fall above the window.
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
        struct item *it = (r == nitems) ? &pending : &items[r];
        item_rows(it, W);
        for (int i = 0; i < it->nrows && at <= body; i++) {
            int need = wrap_count(it->rows[i], W);
            if (skip >= need) {
                skip -= need;
                continue;
            }
            // A row only partly above the window would need a mid-row start;
            // the window is sized so this only trims the oldest entry, and
            // starting it whole is close enough to keep simple.
            skip = 0;
            at += paint_row(it->rows[i], at, body - at + 1, W);
        }
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
        struct item *it = items_push();
        if (it)
            rows_set(it, chrome_rows[i], 0);
        free(chrome_rows[i]);
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

void viewport_begin(void)
{
    if (active)
        return;
    active = 1;
    direct_str("\x1b[?1049h");
    direct_str(MOUSE_ON);
    fflush(stdout);
    viewport_paint();
}

void viewport_end(void)
{
    if (!active)
        return;
    active = 0;
    suspended = 0;
    direct_str(MOUSE_OFF);
    direct_str("\x1b[?25h");
    direct_str("\x1b[?1049l");
    fflush(stdout);
}

void viewport_dump(void)
{
    int W = tty_screen_columns();
    for (int i = 0; i < nitems; i++) {
        item_rows(&items[i], W);
        for (int r = 0; r < items[i].nrows; r++) {
            direct_str(items[i].rows[r]);
            direct_str("\x1b[0m\r\n");
        }
    }
    if (open_len) {
        direct(open_buf, open_len);
        direct_str("\x1b[0m\r\n");
    }
    fflush(stdout);
}

void viewport_suspend(void)
{
    if (!active || suspended)
        return;
    suspended = 1;
    direct_str(MOUSE_OFF);
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
    direct_str(MOUSE_ON);
    fflush(stdout);
    viewport_paint();
}
