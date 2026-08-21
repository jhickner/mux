#include "viewport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"
#include "tty.h"
#include "ui.h"
#include "vendor/cJSON.h"

#define ITEMS_KEEP 8000

// A printed thing. No render means raw output, soft-wrapped only, with cols 0.
struct item {
    viewport_render_fn render;
    void  *ud;
    void (*free_ud)(void *);
    int    reflow;              /* render() again at a new width */
    char **rows;
    int    nrows;
    int    cols;
    unsigned id;
    const char        *kind;    /* names the loader that rebuilds it */
    viewport_encode_fn encode;
};

static struct item *items;
static int    nitems, items_cap;
static unsigned next_id = 1;

static char  *open_buf;
static size_t open_len, open_cap;
static viewport_render_fn open_render;
static void  *open_ud;
static void (*open_free)(void *);
static int    open_reflow;
static int    open_wrapped;
static int    open_pad_after;   /* blank rows the entry being written wants below it */
static int    open_paid;        /* its seam was settled when it was opened */

// Owed by the last entry closed, and paid at the seam with whatever comes
// next. Part of a screen's state: each session keeps its own.
static int    tail_pad;

// Renderers nest: an md entry can place an image, which opens an item of its
// own. Only the outermost one owns an entry; the inner ones write into it.
#define OPEN_MAX 4

struct open_frame {
    void  *ud;
    void (*free_ud)(void *);
};

static struct open_frame open_stack[OPEN_MAX];
static int open_depth;

// Set while an entry's render callback is on the stack. Renderers are pure
// with respect to the store: nothing they do may append, drop or move an
// entry, because measuring and painting hold indices into it.
static int in_render;

static char **chrome_rows;
static int    chrome_n, chrome_cap;
static int    chrome_caret_row, chrome_caret_col = -1;

static int held;
static int active;
static int handed;              /* the alt screen was left up for a successor */
static int suspended;
static int scrolled;
static int dirty;

// Where the window sits when it is scrolled back: the entry pinned at the top
// and how many of its rows fall above it. The offset from the end is derived
// from these, so output appended below moves under the window instead of
// dragging it to the bottom.
static unsigned anchor_id;
static int      anchor_skip;

// Button reporting only, so shift-drag still selects text.
#define MOUSE_ON  "\x1b[?1000h\x1b[?1006h"
#define MOUSE_OFF "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l"

// A frame goes out inside a synchronized update, so the terminal shows it
// whole rather than mid-diff. Not under tmux: tmux answers the end of one by
// redrawing the entire pane, which turns every frame — every spinner tick —
// into a full repaint, and that is the flicker the update was meant to avoid.
// tmux batches what it sends its client anyway.
static int sync_frames(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("TMUX") == NULL;
    return on;
}

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

int viewport_active(void) { return active && !suspended; }

void viewport_touch(void) { dirty = 1; }

int viewport_scrolled(void) { return scrolled; }

unsigned viewport_mark(void) { return next_id; }

// Ids run in order: appended at the end, dropped from the front.
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

static int index_of_mark(unsigned mark)
{
    int lo = 0, hi = nitems - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (items[mid].id == mark)
            return mid;
        if (items[mid].id < mark)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    // Dropped off the front, or the entry still being written: hold at the
    // oldest row, or at the end of the stream.
    return mark >= next_id ? nitems : 0;
}

void *viewport_item_data(unsigned mark)
{
    struct item *it = item_by_mark(mark);
    return it ? it->ud : NULL;
}

void viewport_item_persist(unsigned mark, const char *kind, viewport_encode_fn encode)
{
    struct item *it = item_by_mark(mark);
    if (!it)
        return;
    it->kind = kind;
    it->encode = encode;
}

void viewport_item_update(unsigned mark)
{
    if (in_render)
        return;
    struct item *it = item_by_mark(mark);
    if (!it || !it->render)
        return;
    it->cols = -1;             /* no width matches: forces a re-render */
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
    if (in_render)
        return NULL;
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

static int row_is_blank(const char *s, size_t n);

// Blank rows already at the end of the store, which a seam counts as paid.
static int trailing_blanks(void)
{
    int n = 0;
    for (int i = nitems - 1; i >= 0; i--) {
        for (int r = items[i].nrows - 1; r >= 0; r--) {
            if (!row_is_blank(items[i].rows[r], strlen(items[i].rows[r])))
                return n;
            n++;
        }
        if (items[i].nrows == 0 && !items[i].render)
            n++;                /* an empty raw line is a blank row */
    }
    return n;
}

static void blank_push(void)
{
    struct item *it = items_push();
    if (it)
        rows_set(it, "", 0);
}

// The seam above an entry about to be pushed. `own` is the blank rows it opens
// with, which count toward the same gap.
static void pad_seam(int before, int own)
{
    if (!nitems)                /* nothing above: the top of the transcript */
        return;
    int want = before > tail_pad ? before : tail_pad;
    want -= trailing_blanks() + own;
    for (int i = 0; i < want; i++)
        blank_push();
}

// Rows nobody opened an entry for: a stray print, or one this module has not
// been taught about yet. They stand as one entry per line, which is what the
// store was before entries were declared at all. Set MUX_STRICT_ENTRIES to
// have them named in <config>/loose-rows.log, so a new one shows up as
// something to declare rather than as spacing that quietly goes wrong.
static void loose_row(const char *body, size_t n)
{
    if (!getenv("MUX_STRICT_ENTRIES"))
        return;
    while (n && (body[n - 1] == '\n' || body[n - 1] == '\r'))
        n--;
    if (!n)
        return;                 /* a blank line separates, it does not print */

    char path[4200];
    if (!path_config_file(path, sizeof path, "loose-rows.log"))
        return;
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "%.*s\n", (int)(n > 120 ? 120 : n), body);
    fclose(f);
}

static void open_close(int cols)
{
    // A wrapped entry paid its seam when it was opened; a raw line pays here,
    // where its own leading blank is finally known.
    if (!open_paid) {
        loose_row(open_buf ? open_buf : "", open_len);
        const char *body = open_buf ? open_buf : "";
        pad_seam(0, row_is_blank(body, open_len) ? 1 : 0);
    }

    struct item *it = items_push();
    if (!it) {
        open_len = 0;
        return;
    }
    it->render = open_render;
    it->ud = open_ud;
    it->free_ud = open_free;
    it->reflow = open_reflow;
    rows_set(it, open_buf ? open_buf : "", cols);

    open_len = 0;
    open_render = NULL;
    open_ud = NULL;
    open_free = NULL;
    open_reflow = 0;
    tail_pad = open_pad_after;
    open_pad_after = 0;
    open_paid = 0;
}

unsigned viewport_item_begin(const struct viewport_entry *e)
{
    static const struct viewport_entry none;
    if (!e)
        e = &none;
    void *ud = e->ud;
    void (*free_ud)(void *) = e->free_ud;

    // Counted past the limit, so begin and end pair up and an overflow cannot
    // pop somebody else's frame.
    // A render callback counts as an enclosing frame: it may not open an entry
    // of its own, so the store cannot move while it is being measured.
    int depth = open_depth++;
    if (depth > 0 || in_render) {
        // Nested: the output belongs to the enclosing entry, which already has
        // a payload, so this one is dropped at the matching end. No entry of
        // its own means no mark, and persist on 0 is a no-op.
        if (depth < OPEN_MAX) {
            open_stack[depth].ud = ud;
            open_stack[depth].free_ud = free_ud;
        } else if (free_ud && ud) {
            free_ud(ud);
        }
        return 0;
    }

    // Rows that never reach the transcript get no entry, and no padding
    // either: it would land in whatever is taking the output instead.
    int diverted = ui_diverted();

    if (viewport_active() && open_len)
        open_close(0);
    if (viewport_active() && !diverted) {
        pad_seam(e->pad_before, 0);
        open_paid = 1;
    } else if (!viewport_active() && !diverted) {
        // Nothing is kept, so the seam is printed as it is reached. Only what
        // is owed on the way in: a pad below the last entry printed has
        // nothing after it to be separated from.
        int want = e->pad_before > tail_pad ? e->pad_before : tail_pad;
        for (int i = 0; i < want; i++)
            ui_put("\n");
        tail_pad = e->pad_after;
    }
    open_render = e->render;
    open_ud = ud;
    open_free = free_ud;
    open_reflow = e->reflow;
    open_pad_after = e->pad_after;

    // With no viewport there is no entry to own the payload; item_end frees it.
    // No entry means no mark either: an id handed out now would be taken by
    // whichever entry is opened next, and the caller would be amending
    // somebody else's payload through it.
    open_wrapped = viewport_active() && !diverted;
    return open_wrapped ? next_id : 0;
}

void viewport_item_end(void)
{
    if (open_depth == 0)
        return;
    int depth = --open_depth;
    if (depth > 0 || in_render) {
        if (depth < OPEN_MAX) {
            struct open_frame *f = &open_stack[depth];
            if (f->free_ud && f->ud)
                f->free_ud(f->ud);
            f->ud = NULL;
            f->free_ud = NULL;
        }
        return;
    }

    if (!open_wrapped) {
        if (open_free && open_ud)
            open_free(open_ud);
        open_render = NULL;
        open_ud = NULL;
        open_free = NULL;
        open_pad_after = 0;
        open_paid = 0;
        return;
    }
    open_wrapped = 0;
    open_close(tty_screen_columns());
    dirty = 1;
}

void viewport_write(const char *s, size_t n)
{
    if (in_render)
        return;
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
            open_append(s + start, i - start);
            open_len = 0;
            start = i + 1;
        }
    }
    open_append(s + start, n - start);

    dirty = 1;
}

// Escapes take no cells, so a row of only styling is blank.
static int row_is_blank(const char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        enum ui_esc_kind kind;
        size_t end = ui_esc_span(s, n, i, &kind);
        if (kind == UI_ESC_TEXT && s[i] != ' ' && s[i] != '\t' && s[i] != '\r')
            return 0;
        i = end;
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

struct viewport_state {
    struct item *items;
    int    nitems, items_cap;
    char  *open_buf;
    size_t open_len, open_cap;
    viewport_render_fn open_render;
    void  *open_ud;
    void (*open_free)(void *);
    int    open_wrapped;
    int    open_pad_after;
    int    open_paid;
    int    tail_pad;
    int    scrolled;
    unsigned anchor_id;
    int      anchor_skip;
};

void viewport_hold(int on)
{
    held = on ? 1 : 0;
}

struct viewport_state *viewport_state_new(void)
{
    return calloc(1, sizeof(struct viewport_state));
}

void viewport_state_free(struct viewport_state *st)
{
    if (!st)
        return;
    for (int i = 0; i < st->nitems; i++)
        item_free(&st->items[i]);
    free(st->items);
    free(st->open_buf);
    if (st->open_free && st->open_ud)
        st->open_free(st->open_ud);
    free(st);
}

void viewport_stash(struct viewport_state *st)
{
    if (!st)
        return;
    st->items = items;
    st->nitems = nitems;
    st->items_cap = items_cap;
    st->open_buf = open_buf;
    st->open_len = open_len;
    st->open_cap = open_cap;
    st->open_render = open_render;
    st->open_ud = open_ud;
    st->open_free = open_free;
    st->open_wrapped = open_wrapped;
    st->open_pad_after = open_pad_after;
    st->open_paid = open_paid;
    st->tail_pad = tail_pad;
    st->scrolled = scrolled;
    st->anchor_id = anchor_id;
    st->anchor_skip = anchor_skip;

    items = NULL;
    nitems = items_cap = 0;
    open_buf = NULL;
    open_len = open_cap = 0;
    open_render = NULL;
    open_ud = NULL;
    open_free = NULL;
    open_wrapped = 0;
    open_pad_after = 0;
    open_paid = 0;
    tail_pad = 0;
    scrolled = 0;
    anchor_id = 0;
    anchor_skip = 0;
    dirty = 1;
}

void viewport_adopt(struct viewport_state *st)
{
    if (!st)
        return;
    items = st->items;
    nitems = st->nitems;
    items_cap = st->items_cap;
    open_buf = st->open_buf;
    open_len = st->open_len;
    open_cap = st->open_cap;
    open_render = st->open_render;
    open_ud = st->open_ud;
    open_free = st->open_free;
    open_wrapped = st->open_wrapped;
    open_pad_after = st->open_pad_after;
    open_paid = st->open_paid;
    tail_pad = st->tail_pad;
    scrolled = st->scrolled;
    anchor_id = st->anchor_id;
    anchor_skip = st->anchor_skip;
    memset(st, 0, sizeof *st);
    dirty = 1;
}

void viewport_clear(void)
{
    for (int i = 0; i < nitems; i++)
        item_free(&items[i]);
    nitems = 0;
    open_len = 0;
    tail_pad = 0;
    scrolled = 0;
    anchor_id = 0;
    anchor_skip = 0;
    dirty = 1;
}

/* --- style carried across a soft wrap ------------------------------------ */

// A soft-wrapped continuation resumes the style the cut left behind. mux emits
// one complete SGR per role, so everything since the last reset is the state.
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

// One escape or one codepoint at `i`: adds its cells, folds its style into st.
static size_t step(const char *s, size_t n, size_t i, size_t *cells, struct style *st)
{
    enum ui_esc_kind kind;
    size_t end = ui_esc_span(s, n, i, &kind);

    switch (kind) {
    case UI_ESC_TEXT:
        *cells += ui_cells_n(s + i, end - i);
        break;
    case UI_ESC_SGR:
        if (st) {
            if (sgr_is_reset(s + i, end - i)) {
                st->len = 0;
                st->buf[0] = '\0';
            } else {
                style_add(st, s + i, end - i);
            }
        }
        break;
    case UI_ESC_OSC8:
        if (st)
            style_add(st, s + i, end - i);
        break;
    case UI_ESC_OTHER:
        break;
    }
    return end;
}

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

// Re-renders the entry if the width changed. Raw entries keep their rows.
static void item_rows(struct item *it, int W)
{
    if (!it->render || !it->reflow || it->cols == W)
        return;
    ui_capture_begin(W);
    in_render++;
    it->render(it->ud, W);
    in_render--;
    char *painted = ui_capture_end();
    rows_set(it, painted ? painted : "", W);
    free(painted);
}

// By index, not by pointer: `items` is only stable across a render because of
// the guard above, and nothing here should depend on that twice over.
static struct item *item_at(int r, struct item *pending)
{
    return r == nitems ? pending : &items[r];
}

static int item_height(int r, struct item *pending, int W)
{
    item_rows(item_at(r, pending), W);
    struct item *it = item_at(r, pending);
    if (it->nrows == 0)
        return it->render ? 0 : 1;
    int used = 0;
    for (int i = 0; i < it->nrows; i++)
        used += wrap_count(it->rows[i], W);
    return used;
}

// What the screen should look like: a string per row, hashed so frames compare
// without comparing bytes. An image row is kilobytes of placeholder cells.
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

// Appends one stored row as the screen rows it draws as at width W.
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

// Unwrapped output short of its newline still shows.
static int window_pending(struct item *pending)
{
    if (open_len && !open_wrapped) {
        pending->rows = &open_buf;
        pending->nrows = 1;
        return 1;
    }
    return 0;
}

// Walks back from the newest entry until the window is covered: cost is the
// size of the window, not of the history. Returns the first entry and its rows.
static int window_first(int W, int body, int scroll, struct item *pending, int total,
                        int *have_out)
{
    int want = body + scroll;
    int first = total;
    int have = 0;
    while (first > 0 && have < want) {
        have += item_height(first - 1, pending, W);
        first--;
    }
    *have_out = have;
    return first;
}

// What the next frame will show. The one answer: a query that disagreed with
// the paint would pin or drop the sticky prompt at the wrong scroll offset.
struct window {
    int first;                  /* index of the topmost entry on screen */
    int skip;                   /* its screen rows that fall above the window */
    int body;                   /* screen rows the transcript gets */
    int chrome_shown;
    int total;
    int scrolled;               /* clamped at the top of the transcript */
};

// `pending` must outlive the result: it stands in for unwrapped output and is
// entry `nitems`.
static struct window window_geometry(int W, int H, struct item *pending)
{
    struct window g = {0};

    int ch = chrome_n;
    if (ch > H)
        ch = H;
    if (ch < 0)
        ch = 0;

    g.total = nitems + window_pending(pending);
    g.scrolled = scrolled;

    // Scrolled back: the offset is whatever holds the anchor where it was,
    // counted over the entries from it to the end of the stream.
    if (anchor_id) {
        int tail = ch;
        for (int r = index_of_mark(anchor_id); r < g.total; r++)
            tail += item_height(r, pending, W);
        int want = tail - anchor_skip - H;
        g.scrolled = want > 0 ? want : 0;
    }

    // The chrome is the end of the stream, not a fixture: it scrolls off, and
    // only what is left over of the scroll moves the transcript.
    int chrome_shown = ch - g.scrolled;
    if (chrome_shown < 0)
        chrome_shown = 0;
    int body = H - chrome_shown;
    int scroll = g.scrolled > ch ? g.scrolled - ch : 0;

    int have = 0;
    int first = window_first(W, body, scroll, pending, g.total, &have);
    if (first == 0 && have < body + scroll) {
        // Past the beginning: the oldest row holds at the top.
        int most = have + ch - H;
        g.scrolled = most > 0 ? most : 0;
        chrome_shown = ch - g.scrolled;
        if (chrome_shown < 0)
            chrome_shown = 0;
        body = H - chrome_shown;
        scroll = g.scrolled > ch ? g.scrolled - ch : 0;
    }

    g.first = first;
    g.body = body;
    g.chrome_shown = chrome_shown;
    g.skip = have - body - scroll;
    if (g.skip < 0)
        g.skip = 0;
    return g;
}

// Recomputed, not read off the last paint, so it is right before the next one.
int viewport_visible(unsigned mark)
{
    int W = tty_screen_columns(), H = tty_rows();
    if (W < 1 || H < 1)
        return 1;

    struct item pending = {0};
    struct window g = window_geometry(W, H, &pending);
    return g.first >= nitems ? mark >= next_id : mark >= items[g.first].id;
}

// How far the frame moved as a whole, so a scroll sends only the rows that
// came into view instead of redrawing the screen.
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

    // Staying put is the baseline: repeated rows (an image) score the same
    // shifted as not, and would scroll for nothing.
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
    if (!active || suspended || held || in_render)
        return;

    int H = tty_rows(), W = tty_screen_columns();
    if (H < 1 || W < 1)
        return;

    struct item pending = {0};
    struct window g = window_geometry(W, H, &pending);
    scrolled = g.scrolled;
    if (scrolled > 0) {
        anchor_id = g.first < nitems ? items[g.first].id : next_id;
        anchor_skip = g.skip;
    } else {
        anchor_id = 0;
        anchor_skip = 0;
    }

    int chrome_shown = g.chrome_shown;
    int body = g.body;
    int skip = g.skip;

    struct frame all = {0};
    for (int r = g.first; r < g.total && r <= nitems; r++) {
        item_rows(item_at(r, &pending), W);
        struct item *it = item_at(r, &pending);
        if (it->nrows == 0 && !it->render)
            frame_push(&all, blank_row());
        for (int i = 0; i < it->nrows; i++)
            row_into_frame(&all, it->rows[i], W);
        if (all.n >= skip + body)
            break;
    }

    frame_reset(&built);

    // A short transcript rests on the bottom rather than hanging from the top.
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

    for (int i = 0; i < chrome_shown; i++)
        frame_push(&built, strdup(chrome_rows[i]));

    // A resize invalidates everything on screen.
    if (shown_rows != H || shown_cols != W) {
        frame_reset(&shown);
        shown_rows = H;
        shown_cols = W;
    }

    if (sync_frames())
        direct_str("\x1b[?2026h");
    direct_str("\x1b[?25l");
    direct_str("\x1b[?7l");

    // Move what the terminal already has; send only what that leaves uncovered.
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

        // Follow the move, so the diff below only names rows it missed.
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

    if (sync_frames())
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
    anchor_id = 0;
    scrolled += delta;
    if (scrolled < 0)
        scrolled = 0;
    dirty = 1;
    viewport_paint();
}

void viewport_scroll_end(void)
{
    anchor_id = 0;
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
    if (!active && !handed)
        return;
    active = 0;
    handed = 0;
    suspended = 0;
    direct_str(MOUSE_OFF);
    direct_str("\x1b[?25h");
    direct_str("\x1b[?1049l");
    fflush(stdout);
}

// Exec'ing a successor: the alt screen stays up, so nothing flashes and the
// terminal's own scrollback is never touched. It restores the entries itself.
void viewport_handoff(void)
{
    if (!active)
        return;
    active = 0;
    handed = 1;
    suspended = 0;
    direct_str(MOUSE_OFF);
    direct_str("\x1b[?25h");
    fflush(stdout);
}

// Started into an alt screen a predecessor left up.
void viewport_inherit(void)
{
    if (active)
        return;
    active = 1;
    direct_str(MOUSE_ON);
    fflush(stdout);
    viewport_forget();
}

// One JSON object per entry. An entry that can say what it is travels as its
// own state and is rebuilt live; the rest travel as rows and come back as raw
// output, soft-wrapped from then on.
static int dump_item(FILE *f, const struct item *it)
{
    cJSON *line = cJSON_CreateObject();
    if (!line)
        return 0;

    char *state = it->encode && it->kind ? it->encode(it->ud) : NULL;
    if (state) {
        cJSON_AddStringToObject(line, "kind", it->kind);
        cJSON_AddItemToObject(line, "state", cJSON_CreateRaw(state));
        free(state);
    } else {
        cJSON *rows = it->nrows > 0
                          ? cJSON_CreateStringArray((const char *const *)it->rows, it->nrows)
                          : cJSON_CreateArray();
        cJSON_AddStringToObject(line, "kind", "rows");
        cJSON_AddItemToObject(line, "rows", rows);
    }

    char *text = cJSON_PrintUnformatted(line);
    cJSON_Delete(line);
    if (!text)
        return 0;
    int ok = fprintf(f, "%s\n", text) > 0;
    free(text);
    return ok;
}

int viewport_dump(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;

    int ok = 1;
    for (int i = 0; i < nitems; i++)
        ok = dump_item(f, &items[i]) && ok;
    if (open_len) {
        struct item tail = {0};
        rows_set(&tail, open_buf, 0);
        ok = dump_item(f, &tail) && ok;
        rows_free(&tail);
    }
    ok = ferror(f) == 0 && ok;
    return fclose(f) == 0 && ok;
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
    viewport_forget();
    viewport_paint();
}
