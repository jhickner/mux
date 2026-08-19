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

// Entries are appended and only ever dropped from the front, so the ids run in
// order and can be searched rather than scanned.
static struct item *item_by_mark(unsigned mark)
{
    int lo = 0, hi = nitems - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (items[mid].id == mark)
            return &items[mid];
        if (items[mid].id < mark)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

void *viewport_item_data(unsigned mark)
{
    struct item *it = item_by_mark(mark);
    return it ? it->ud : NULL;
}

void viewport_item_update(unsigned mark)
{
    struct item *it = item_by_mark(mark);
    if (!it || !it->render)
        return;
    // Nothing is a valid width, so the next paint has to render it again.
    it->cols = -1;
    dirty = 1;
    viewport_paint();
}

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

unsigned viewport_item_begin(viewport_render_fn render, void *ud, void (*free_ud)(void *))
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
    return next_id;
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

// Escapes take no cells, so a row of nothing but styling is still blank.
static int row_is_blank(const char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        if (s[i] == 0x1b) {
            size_t j = i + 1;
            if (j < n && s[j] == '[') {
                for (j++; j < n && (s[j] < '@' || s[j] > '~'); j++)
                    ;
            } else if (j < n && (s[j] == ']' || s[j] == 'P' || s[j] == '_' ||
                                 s[j] == '^' || s[j] == 'X')) {
                for (j++; j < n; j++)
                    if (s[j] == 0x07 || (s[j] == 0x1b && j + 1 < n && s[j + 1] == '\\'))
                        break;
                if (j < n && s[j] == 0x1b)
                    j++;
            }
            i = j < n ? j + 1 : n;
            continue;
        }
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r')
            return 0;
        i++;
    }
    return 1;
}

static int text_ends_blank(const char *s, size_t n)
{
    if (!n)
        return 1;
    if (s[n - 1] == '\n')       /* the terminator of the last row, not a row */
        n--;
    size_t start = n;
    while (start && s[start - 1] != '\n')
        start--;
    return row_is_blank(s + start, n - start);
}

int viewport_ends_blank(void)
{
    if (!viewport_active())
        return 0;
    if (open_len)
        return text_ends_blank(open_buf, open_len);
    for (int i = nitems - 1; i >= 0; i--) {
        if (items[i].nrows == 0)
            continue;
        const char *last = items[i].rows[items[i].nrows - 1];
        return row_is_blank(last, strlen(last));
    }
    return 1;
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

// A frame is what the screen should look like: one string per screen row, and
// a hash of each so two frames can be compared without comparing their bytes.
// An image row is a couple of kilobytes of placeholder cells, so what a paint
// costs is decided by how much of the frame it can leave alone.
struct frame {
    char              **row;
    unsigned long long *hash;
    int                 n, cap;
};

static struct frame shown;      /* what the terminal is displaying */
static struct frame built;      /* what it should display */
static int shown_rows, shown_cols;

static unsigned long long row_hash(const char *s)
{
    unsigned long long h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

static void frame_reset(struct frame *f)
{
    for (int i = 0; i < f->n; i++)
        free(f->row[i]);
    f->n = 0;
}

// Takes ownership of `s`.
static void frame_push(struct frame *f, char *s)
{
    if (!s)
        return;
    if (f->n == f->cap) {
        int cap = f->cap ? f->cap * 2 : 64;
        char **r = realloc(f->row, (size_t)cap * sizeof *r);
        unsigned long long *h = realloc(f->hash, (size_t)cap * sizeof *h);
        if (r)
            f->row = r;
        if (h)
            f->hash = h;
        if (!r || !h) {
            free(s);
            return;
        }
        f->cap = cap;
    }
    f->hash[f->n] = row_hash(s);
    f->row[f->n++] = s;
}

static void frame_swap(struct frame *a, struct frame *b)
{
    struct frame t = *a;
    *a = *b;
    *b = t;
}

// Splits one stored row into the screen rows it draws as at width W, each
// carrying the style the cut before it left behind, and appends them.
static void row_into_frame(struct frame *f, const char *s, int W)
{
    size_t n = strlen(s);
    struct style st = {{0}, 0};
    size_t i = 0, start = 0;

    for (;;) {
        size_t cells = 0;
        size_t line_start = start;
        struct style at_start = st;
        while (i < n) {
            size_t was = cells;
            size_t next = step(s, n, i, &cells, &st);
            if (cells > (size_t)W && was > 0)
                break;
            i = next;
        }

        size_t body = i - line_start;
        char  *out = malloc(at_start.len + body + 1);
        if (!out)
            return;
        memcpy(out, at_start.buf, at_start.len);
        memcpy(out + at_start.len, s + line_start, body);
        out[at_start.len + body] = '\0';
        frame_push(f, out);

        start = i;
        if (i >= n)
            return;
    }
}

static char *blank_row(void) { return strdup(""); }

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
static int window_first(int W, int body, int scroll, struct item *pending, int total,
                        int *have_out)
{
    int want = body + scroll;
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
    int first = window_first(W, body_rows(), scrolled, &pending, total, &have);
    return first >= nitems ? mark >= next_id : mark >= items[first].id;
}

// How far the frame moved as a whole. Scrolling shifts every row, so without
// this a wheel tick would redraw the screen; with it the terminal moves what
// it already has and only the rows that came into view are sent.
static int shift_score(int body, int k)
{
    int score = 0;
    for (int i = 0; i < body; i++) {
        int j = i + k;
        if (j < 0 || j >= body)
            continue;
        if (built.hash[i] == shown.hash[j] && built.row[i][0])
            score++;
    }
    return score;
}

static int frame_shift(int body, int *score_out)
{
    *score_out = 0;
    if (shown.n < body)
        return 0;

    // Staying put is the baseline a shift has to beat. Without it a screen of
    // repeated rows — an image is exactly that — scores just as well shifted
    // as not, and the frame would be scrolled for nothing.
    int best = shift_score(body, 0);
    int best_k = 0;
    for (int k = -(body - 1); k < body; k++) {
        if (k == 0)
            continue;
        int score = shift_score(body, k);
        if (score > best) {
            best = score;
            best_k = k;
        }
    }
    *score_out = best_k ? best : 0;
    return best_k;
}

void viewport_forget(void)
{
    frame_reset(&shown);
}

void viewport_paint(void)
{
    if (!active || suspended)
        return;

    int H = tty_rows(), W = tty_screen_columns();
    if (H < 1 || W < 1)
        return;

    int ch = chrome_n;
    if (ch > H)
        ch = H;
    if (ch < 0)
        ch = 0;

    struct item pending = {0};
    int total = nitems + window_pending(&pending);

    // The chrome is the end of the stream, not a fixture on the screen:
    // scrolling back pushes it off the bottom the way it pushes anything else.
    int chrome_shown = ch - scrolled;
    if (chrome_shown < 0)
        chrome_shown = 0;
    int body = H - chrome_shown;
    int scroll = scrolled > ch ? scrolled - ch : 0;

    int have = 0;
    int first = window_first(W, body, scroll, &pending, total, &have);
    if (first == 0 && have < body + scroll) {
        // Scrolled past the beginning: the oldest row holds at the top.
        int most = have + ch - H;
        scrolled = most > 0 ? most : 0;
        chrome_shown = ch - scrolled;
        if (chrome_shown < 0)
            chrome_shown = 0;
        body = H - chrome_shown;
        scroll = scrolled > ch ? scrolled - ch : 0;
    }

    // Screen rows of the first entry that fall above the window.
    int skip = have - body - scroll;
    if (skip < 0)
        skip = 0;

    // Every screen row the window could show, then the slice of it that fits.
    struct frame all = {0};
    for (int r = first; r < total; r++) {
        struct item *it = (r == nitems) ? &pending : &items[r];
        item_rows(it, W);
        if (it->nrows == 0 && !it->render)
            frame_push(&all, blank_row());
        for (int i = 0; i < it->nrows; i++)
            row_into_frame(&all, it->rows[i], W);
        if (all.n >= skip + body)
            break;
    }

    frame_reset(&built);

    // A transcript shorter than the window sits on the bottom of it, against
    // the chrome, rather than hanging from the top of the screen.
    int content = all.n - skip;
    if (content < 0)
        content = 0;
    if (content > body)
        content = body;
    for (int i = content; i < body; i++)
        frame_push(&built, blank_row());
    for (int i = 0; i < content; i++)
        frame_push(&built, strdup(all.row[skip + i]));

    frame_reset(&all);
    free(all.row);
    free(all.hash);

    // Scrolling back takes the chrome off the bottom a row at a time, so what
    // is left of it is its first rows.
    for (int i = 0; i < chrome_shown; i++)
        frame_push(&built, strdup(chrome_rows[i]));

    // A resize invalidates everything the terminal is showing.
    if (shown_rows != H || shown_cols != W) {
        frame_reset(&shown);
        shown_rows = H;
        shown_cols = W;
    }

    direct_str("\x1b[?2026h");
    direct_str("\x1b[?25l");
    direct_str("\x1b[?7l");

    // Let the terminal move the rows it already has, and send only what that
    // leaves uncovered.
    // The chrome moves with everything else now, so the whole screen is one
    // thing to shift rather than a body with a fixture under it.
    int span = built.n;
    int score = 0;
    int k = shown.n == built.n ? frame_shift(span, &score) : 0;
    if (k != 0 && score >= span / 3) {
        char esc[32];
        snprintf(esc, sizeof esc, "\x1b[1;%dr", span);
        direct_str(esc);
        snprintf(esc, sizeof esc, "\x1b[%d%c", k > 0 ? k : -k, k > 0 ? 'S' : 'T');
        direct_str(esc);
        direct_str("\x1b[r");

        // Follow the move in the record of what is displayed, so the diff
        // below only names rows the scroll did not put right.
        for (int i = 0; i < span; i++) {
            int j = k > 0 ? i : span - 1 - i;
            int from = j + k;
            free(shown.row[j]);
            if (from >= 0 && from < span) {
                shown.row[j] = strdup(shown.row[from]);
                shown.hash[j] = shown.hash[from];
            } else {
                shown.row[j] = blank_row();
                shown.hash[j] = row_hash("");
            }
        }
    }

    for (int i = 0; i < built.n; i++) {
        if (i < shown.n && shown.hash[i] == built.hash[i] &&
            strcmp(shown.row[i], built.row[i]) == 0)
            continue;
        cup(i + 1, 1);
        direct_str("\x1b[0m\x1b[K");
        direct_str(built.row[i]);
    }

    direct_str("\x1b[?7h");
    direct_str("\x1b[0m");

    if (chrome_caret_col >= 0 && chrome_caret_row >= 0 && chrome_caret_row < chrome_shown) {
        int col = chrome_caret_col + 1;
        if (col > W)
            col = W;
        cup(body + 1 + chrome_caret_row, col);
        direct_str("\x1b[?25h");
    }

    direct_str("\x1b[?2026l");
    fflush(stdout);

    frame_swap(&shown, &built);
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
    // Whatever had the screen left it in a state this knows nothing about.
    viewport_forget();
    viewport_paint();
}
