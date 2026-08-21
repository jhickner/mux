/**
 * mdv2.h - Markdown -> Telegram MarkdownV2, single-header (stb style)
 *
 * Telegram's MarkdownV2 rejects a message outright (HTTP 400) if a reserved
 * character appears unescaped or an entity is left open, so the usual trick of
 * "escape a few characters and hope" fails on real LLM output. This converter
 * instead parses a markdown subset and re-emits it: every literal character is
 * escaped, and formatting markers are only ever emitted by the writer, never
 * copied from the source. Output is therefore always well-formed; an unmatched
 * delimiter in the input degrades to literal text rather than a 400.
 *
 * Supported: fenced code, inline code, **bold**, *italic*, _italic_,
 * ~~strike~~, [text](url), headings, blockquotes, bullet/ordered lists,
 * horizontal rules, and tables (rendered as an aligned code block).
 *
 * USAGE:
 *   char *out = mdv2_convert(markdown);   // caller frees
 *
 * Also splits long input on markdown-safe boundaries, since cutting a message
 * mid-entity would break the parse:
 *   size_t n; char **parts = mdv2_split(markdown, 3500, &n);
 */

#ifndef MDV2_H
#define MDV2_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Convert markdown to MarkdownV2. Returns a malloc'd string (caller frees), or
 * NULL on allocation failure. */
static char *mdv2_convert(const char *src);

/* Split markdown into chunks of at most `limit` bytes, breaking only at
 * top-level line boundaries and never inside a fenced code block (a block that
 * must be cut is closed and reopened). Returns a malloc'd array of malloc'd
 * strings; *count gets the length. Free each part, then the array. */
static char **mdv2_split(const char *src, size_t limit, size_t *count);

/* Split and convert in one step: returns ready-to-send MarkdownV2 messages,
 * each at most max_len bytes. Escaping can nearly double a chunk's length, so
 * the split is retried against the converted size rather than the source. */
static char **mdv2_messages(const char *src, size_t max_len, size_t *count);

/* ---- growable buffer ---------------------------------------------------- */

typedef struct { char *p; size_t n, cap; } mdv2_buf;

static void mdv2_add(mdv2_buf *b, const char *s, size_t n) {
    if (!n) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1) cap *= 2;
        char *q = (char *)realloc(b->p, cap);
        if (!q) return;               /* OOM: drop the append, stay valid */
        b->p = q; b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}
static void mdv2_puts(mdv2_buf *b, const char *s) { mdv2_add(b, s, strlen(s)); }
static void mdv2_putc(mdv2_buf *b, char c) { mdv2_add(b, &c, 1); }

/* ---- escaping ----------------------------------------------------------- */

/* Reserved in MarkdownV2 body text; all must be backslash-escaped. */
#define MDV2_SPECIAL "_*[]()~`>#+-=|{}.!\\"

static void mdv2_esc(mdv2_buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] && strchr(MDV2_SPECIAL, s[i])) mdv2_putc(b, '\\');
        mdv2_putc(b, s[i]);
    }
}

/* Inside code / pre only ` and \ are reserved. */
static void mdv2_esc_code(mdv2_buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '`' || s[i] == '\\') mdv2_putc(b, '\\');
        mdv2_putc(b, s[i]);
    }
}

/* Inside a link target only ) and \ are reserved. */
static void mdv2_esc_url(mdv2_buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == ')' || s[i] == '\\') mdv2_putc(b, '\\');
        mdv2_putc(b, s[i]);
    }
}

/* ---- inline ------------------------------------------------------------- */

static void mdv2_inline(mdv2_buf *b, const char *s, size_t n);

/* Index of the next `d` (length dlen) in s[from..n), skipping over source
 * backslash escapes and code spans, or -1 if there is none. */
static long mdv2_find(const char *s, size_t n, size_t from,
                      const char *d, size_t dlen) {
    for (size_t i = from; i + dlen <= n; i++) {
        if (s[i] == '\\') { i++; continue; }
        if (s[i] == '`' && d[0] != '`') {
            size_t j = i + 1;
            while (j < n && s[j] != '`') j++;
            if (j < n) { i = j; continue; }     /* unterminated: treat as text */
        }
        if (!memcmp(s + i, d, dlen)) return (long)i;
    }
    return -1;
}

/* A `_` only opens/closes emphasis at a word boundary, so snake_case names and
 * __dunder__ identifiers stay literal. */
static int mdv2_word(char c) { return isalnum((unsigned char)c) || c == '_'; }

static void mdv2_emph(mdv2_buf *b, char out, const char *s, size_t n) {
    mdv2_putc(b, out);
    mdv2_inline(b, s, n);
    mdv2_putc(b, out);
}

static void mdv2_inline(mdv2_buf *b, const char *s, size_t n) {
    size_t i = 0, run = 0;
    while (i < n) {
        size_t adv = 0;             /* set when a construct was consumed */
        long close;
        char c = s[i];

        /* A backslash escapes punctuation only; in C:\Users\x or \n it is
         * literal text. */
        if (c == '\\' && i + 1 < n && ispunct((unsigned char)s[i + 1])) {
            mdv2_esc(b, s + run, i - run);
            mdv2_esc(b, s + i + 1, 1);
            adv = 2;
        } else if (c == '`') {                           /* inline code */
            size_t k = 0;
            while (i + k < n && s[i + k] == '`') k++;
            char fence[8];
            size_t fk = k < sizeof fence ? k : sizeof fence - 1;
            memset(fence, '`', fk);
            close = mdv2_find(s, n, i + k, fence, fk);
            if (close >= 0) {
                mdv2_esc(b, s + run, i - run);
                mdv2_putc(b, '`');
                mdv2_esc_code(b, s + i + k, (size_t)close - (i + k));
                mdv2_putc(b, '`');
                adv = (size_t)close + fk - i;
            }
        } else if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] == c) {
            const char d[3] = { c, c, 0 };               /* **bold** / __bold__ */
            int ok = (c != '_') || i == 0 || !mdv2_word(s[i - 1]);
            close = ok ? mdv2_find(s, n, i + 2, d, 2) : -1;
            /* __x__ is far more often a dunder identifier than bold; only take
             * it when the span is a phrase. */
            if (c == '_' && close > 0 &&
                !memchr(s + i + 2, ' ', (size_t)close - (i + 2))) close = -1;
            if (close > (long)i + 2) {
                mdv2_esc(b, s + run, i - run);
                mdv2_emph(b, '*', s + i + 2, (size_t)close - (i + 2));
                adv = (size_t)close + 2 - i;
            }
        } else if (c == '~' && i + 1 < n && s[i + 1] == '~') {
            close = mdv2_find(s, n, i + 2, "~~", 2);
            if (close > (long)i + 2) {
                mdv2_esc(b, s + run, i - run);
                mdv2_emph(b, '~', s + i + 2, (size_t)close - (i + 2));
                adv = (size_t)close + 2 - i;
            }
        } else if (c == '|' && i + 1 < n && s[i + 1] == '|') {
            close = mdv2_find(s, n, i + 2, "||", 2);
            if (close > (long)i + 2) {
                mdv2_esc(b, s + run, i - run);
                mdv2_puts(b, "||");
                mdv2_inline(b, s + i + 2, (size_t)close - (i + 2));
                mdv2_puts(b, "||");
                adv = (size_t)close + 2 - i;
            }
        } else if (c == '*' || c == '_') {               /* *italic* / _italic_ */
            int ok = !isspace((unsigned char)s[i + 1]) && i + 1 < n;
            if (c == '_' && i > 0 && mdv2_word(s[i - 1])) ok = 0;
            const char d[2] = { c, 0 };
            close = ok ? mdv2_find(s, n, i + 1, d, 1) : -1;
            /* the closer must hug the text and, for _, end a word */
            while (close > 0 && (isspace((unsigned char)s[close - 1]) ||
                                 (close + 1 < (long)n && s[close + 1] == c) ||
                                 (c == '_' && close + 1 < (long)n &&
                                  mdv2_word(s[close + 1]))))
                close = mdv2_find(s, n, (size_t)close + 1, d, 1);
            if (close > (long)i + 1) {
                mdv2_esc(b, s + run, i - run);
                mdv2_emph(b, '_', s + i + 1, (size_t)close - (i + 1));
                adv = (size_t)close + 1 - i;
            }
        } else if (c == '[') {                           /* [text](url) */
            long end = mdv2_find(s, n, i + 1, "]", 1);
            if (end > 0 && end + 1 < (long)n && s[end + 1] == '(') {
                /* balance parens so .../Foo_(bar) keeps its tail */
                long url = -1;
                int depth = 1;
                for (size_t j = (size_t)end + 2; j < n; j++) {
                    if (s[j] == '\\') { j++; continue; }
                    if (s[j] == '(') depth++;
                    else if (s[j] == ')' && --depth == 0) { url = (long)j; break; }
                    else if (isspace((unsigned char)s[j])) break;
                }
                if (url > 0) {
                    mdv2_esc(b, s + run, i - run);
                    mdv2_putc(b, '[');
                    mdv2_inline(b, s + i + 1, (size_t)end - (i + 1));
                    mdv2_puts(b, "](");
                    mdv2_esc_url(b, s + end + 2, (size_t)url - (end + 2));
                    mdv2_putc(b, ')');
                    adv = (size_t)url + 1 - i;
                }
            }
        }

        if (adv) { i += adv; run = i; } else i++;
    }
    mdv2_esc(b, s + run, n - run);
}

/* ---- blocks ------------------------------------------------------------- */

typedef struct { const char *p; size_t n; } mdv2_line;

static mdv2_line mdv2_next_line(const char **cur, const char *end) {
    const char *s = *cur;
    const char *nl = (const char *)memchr(s, '\n', (size_t)(end - s));
    mdv2_line l;
    l.p = s;
    l.n = nl ? (size_t)(nl - s) : (size_t)(end - s);
    *cur = nl ? nl + 1 : end;
    if (l.n && l.p[l.n - 1] == '\r') l.n--;
    return l;
}

static size_t mdv2_indent(mdv2_line l) {
    size_t i = 0;
    while (i < l.n && (l.p[i] == ' ' || l.p[i] == '\t')) i++;
    return i;
}

/* Length of the opening fence run (``` or ~~~) at the start of `l`, else 0. */
static size_t mdv2_fence(mdv2_line l) {
    size_t i = mdv2_indent(l);
    if (i + 3 > l.n) return 0;
    char c = l.p[i];
    if (c != '`' && c != '~') return 0;
    size_t k = 0;
    while (i + k < l.n && l.p[i + k] == c) k++;
    return k >= 3 ? k : 0;
}

static int mdv2_blank(mdv2_line l) { return mdv2_indent(l) == l.n; }

static int mdv2_is_rule(mdv2_line l) {
    size_t i = mdv2_indent(l), k = 0;
    if (i >= l.n) return 0;
    char c = l.p[i];
    if (c != '-' && c != '*' && c != '_') return 0;
    for (; i < l.n; i++) {
        if (l.p[i] == c) k++;
        else if (l.p[i] != ' ') return 0;
    }
    return k >= 3;
}

/* A table separator row: | --- | :--: | ... */
static int mdv2_is_sep(mdv2_line l) {
    int dash = 0;
    size_t i = mdv2_indent(l);
    if (i >= l.n) return 0;
    for (; i < l.n; i++) {
        if (l.p[i] == '-') dash = 1;
        else if (!strchr("|: \t", l.p[i])) return 0;
    }
    return dash;
}

/* Split a table row into trimmed cells; returns the cell count (<= max). */
static int mdv2_cells(mdv2_line l, char **out, int max) {
    const char *s = l.p, *e = l.p + l.n;
    while (s < e && (*s == ' ' || *s == '\t')) s++;
    if (s < e && *s == '|') s++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    if (e > s && e[-1] == '|') e--;
    int n = 0;
    while (s <= e && n < max) {
        const char *bar = s;
        while (bar < e && *bar != '|') {
            if (*bar == '\\' && bar + 1 < e) bar++;
            bar++;
        }
        const char *a = s, *b = bar;
        while (a < b && (*a == ' ' || *a == '\t')) a++;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
        out[n] = (char *)malloc((size_t)(b - a) + 1);
        if (!out[n]) break;
        memcpy(out[n], a, (size_t)(b - a));
        out[n][b - a] = '\0';
        n++;
        if (bar >= e) break;
        s = bar + 1;
    }
    return n;
}

/* Strip inline markdown to plain text, in place: a pre block cannot render
 * entities, so markers would otherwise show up as literal characters. */
static void mdv2_strip(char *s) {
    size_t n = strlen(s), j = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n && ispunct((unsigned char)s[i + 1])) {
            s[j++] = s[++i];
        } else if (s[i] == '[') {          /* [text](url) -> text */
            char *rest = s + i;
            char *close = strstr(rest, "](");
            char *paren = close ? strchr(close, ')') : NULL;
            if (close && paren) {
                size_t tlen = (size_t)(close - rest) - 1;
                memmove(s + j, rest + 1, tlen);
                j += tlen;
                i = (size_t)(paren - s);
            } else s[j++] = s[i];
        } else if (s[i] == '`' || s[i] == '*' ||
                   (s[i] == '~' && i + 1 < n && s[i + 1] == '~')) {
            if (s[i] == '~') i++;          /* drop the marker */
        } else {
            s[j++] = s[i];
        }
    }
    s[j] = '\0';
}

/* Display width of a UTF-8 string, counting each codepoint as one column. */
static size_t mdv2_width(const char *s) {
    size_t w = 0;
    for (; *s; s++) if ((*s & 0xC0) != 0x80) w++;
    return w;
}

#define MDV2_MAX_COLS 12
#define MDV2_MAX_ROWS 64

/* Render a markdown table as a fixed-width code block: proportional fonts and
 * Telegram's lack of table entities make column alignment impossible in body
 * text, but a pre block is monospaced. */
static void mdv2_table(mdv2_buf *b, mdv2_line *rows, int nrows) {
    char *cell[MDV2_MAX_ROWS][MDV2_MAX_COLS] = {{0}};
    int ncell[MDV2_MAX_ROWS] = {0};
    size_t w[MDV2_MAX_COLS] = {0};
    int cols = 0;

    for (int r = 0; r < nrows; r++) {
        ncell[r] = mdv2_cells(rows[r], cell[r], MDV2_MAX_COLS);
        if (ncell[r] > cols) cols = ncell[r];
        for (int i = 0; i < ncell[r]; i++) mdv2_strip(cell[r][i]);
        for (int i = 0; i < ncell[r]; i++) {
            size_t cw = mdv2_width(cell[r][i]);
            if (cw > w[i]) w[i] = cw;
        }
    }

    mdv2_puts(b, "```\n");
    for (int r = 0; r < nrows; r++) {
        for (int i = 0; i < cols; i++) {
            const char *t = i < ncell[r] ? cell[r][i] : "";
            mdv2_esc_code(b, t, strlen(t));
            if (i + 1 < cols) {
                for (size_t k = mdv2_width(t); k < w[i]; k++) mdv2_putc(b, ' ');
                mdv2_puts(b, "  ");
            }
        }
        mdv2_putc(b, '\n');
        if (r == 0) {                       /* rule under the header */
            for (int i = 0; i < cols; i++) {
                for (size_t k = 0; k < w[i]; k++) mdv2_putc(b, '-');
                if (i + 1 < cols) mdv2_puts(b, "  ");
            }
            mdv2_putc(b, '\n');
        }
    }
    mdv2_puts(b, "```\n");

    for (int r = 0; r < nrows; r++)
        for (int i = 0; i < ncell[r]; i++) free(cell[r][i]);
}

static char *mdv2_convert(const char *src) {
    mdv2_buf b = {0};
    if (!src) src = "";
    const char *cur = src, *end = src + strlen(src);

    while (cur < end) {
        const char *line_start = cur;
        mdv2_line l = mdv2_next_line(&cur, end);
        size_t ind = mdv2_indent(l);
        size_t k = mdv2_fence(l);

        if (k) {                                          /* fenced code */
            char fc = l.p[ind];
            const char *lang = l.p + ind + k;
            size_t lang_n = l.n - ind - k;
            while (lang_n && isspace((unsigned char)*lang)) { lang++; lang_n--; }
            mdv2_puts(&b, "```");
            for (size_t i = 0; i < lang_n; i++)
                if (isalnum((unsigned char)lang[i]) || lang[i] == '+' ||
                    lang[i] == '#' || lang[i] == '-')
                    mdv2_putc(&b, lang[i]);
            mdv2_putc(&b, '\n');
            while (cur < end) {
                mdv2_line c = mdv2_next_line(&cur, end);
                size_t ck = mdv2_fence(c);
                if (ck >= k && c.p[mdv2_indent(c)] == fc) break;
                mdv2_esc_code(&b, c.p, c.n);
                mdv2_putc(&b, '\n');
            }
            mdv2_puts(&b, "```\n");
            continue;
        }

        if (mdv2_blank(l)) { mdv2_putc(&b, '\n'); continue; }

        if (mdv2_is_rule(l)) { mdv2_puts(&b, "\\-\\-\\-\\-\\-\\-\\-\\-\n"); continue; }

        /* table: a row of cells followed by a separator row */
        if (memchr(l.p, '|', l.n)) {
            const char *peek = cur;
            if (peek < end) {
                mdv2_line sep = mdv2_next_line(&peek, end);
                if (mdv2_is_sep(sep) && memchr(sep.p, '|', sep.n)) {
                    mdv2_line rows[MDV2_MAX_ROWS];
                    int n = 0;
                    rows[n++] = l;
                    cur = peek;
                    while (cur < end && n < MDV2_MAX_ROWS) {
                        const char *save = cur;
                        mdv2_line r = mdv2_next_line(&cur, end);
                        if (!memchr(r.p, '|', r.n)) { cur = save; break; }
                        rows[n++] = r;
                    }
                    mdv2_table(&b, rows, n);
                    continue;
                }
            }
        }

        if (ind < l.n && l.p[ind] == '#') {               /* heading */
            size_t h = ind;
            while (h < l.n && l.p[h] == '#') h++;
            if (h - ind <= 6 && h < l.n && l.p[h] == ' ') {
                while (h < l.n && l.p[h] == ' ') h++;
                mdv2_putc(&b, '*');
                mdv2_inline(&b, l.p + h, l.n - h);
                mdv2_puts(&b, "*\n");
                continue;
            }
        }

        if (ind < l.n && l.p[ind] == '>') {               /* blockquote */
            size_t q = ind + 1;
            if (q < l.n && l.p[q] == ' ') q++;
            mdv2_add(&b, l.p, ind);
            mdv2_putc(&b, '>');
            mdv2_inline(&b, l.p + q, l.n - q);
            mdv2_putc(&b, '\n');
            continue;
        }

        /* bullet list: -, *, + followed by a space */
        if (ind + 1 < l.n && strchr("-*+", l.p[ind]) && l.p[ind + 1] == ' ') {
            size_t t = ind + 2;
            while (t < l.n && l.p[t] == ' ') t++;
            mdv2_add(&b, l.p, ind);
            mdv2_puts(&b, ind ? "\xe2\x97\xa6 " : "\xe2\x80\xa2 ");   /* ◦ / • */
            mdv2_inline(&b, l.p + t, l.n - t);
            mdv2_putc(&b, '\n');
            continue;
        }

        /* ordered list: digits followed by . or ) and a space */
        {
            size_t d = ind;
            while (d < l.n && isdigit((unsigned char)l.p[d])) d++;
            if (d > ind && d + 1 < l.n && (l.p[d] == '.' || l.p[d] == ')') &&
                l.p[d + 1] == ' ') {
                size_t t = d + 2;
                while (t < l.n && l.p[t] == ' ') t++;
                mdv2_add(&b, l.p, ind);
                mdv2_add(&b, l.p + ind, d - ind);
                mdv2_puts(&b, "\\. ");
                mdv2_inline(&b, l.p + t, l.n - t);
                mdv2_putc(&b, '\n');
                continue;
            }
        }

        (void)line_start;
        mdv2_inline(&b, l.p, l.n);
        mdv2_putc(&b, '\n');
    }

    if (!b.p) b.p = (char *)calloc(1, 1);
    return b.p;
}

/* ---- splitting ---------------------------------------------------------- */

static char *mdv2_dup(const char *s, size_t n) {
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static char **mdv2_split(const char *src, size_t limit, size_t *count) {
    size_t cap = 4, n = 0;
    char **parts = (char **)malloc(cap * sizeof *parts);
    *count = 0;
    if (!parts) return NULL;
    if (!src) src = "";
    if (limit < 64) limit = 64;

    const char *end = src + strlen(src);
    const char *cur = src, *chunk = src;
    const char *fence_line = NULL;    /* opening fence, while inside a block */
    size_t fence_n = 0;
    const char *safe = NULL;          /* last line boundary outside a block */
    char *prefix = NULL;              /* fence header carried into the next part */

    for (;;) {
        int done = cur >= end;
        if (!done) {
            const char *line_start = cur;
            mdv2_line l = mdv2_next_line(&cur, end);
            if (mdv2_fence(l)) {
                if (!fence_line) { fence_line = line_start; fence_n = l.n; }
                else fence_line = NULL;
            }
            if (!fence_line) safe = cur;
            if ((size_t)(cur - chunk) + (prefix ? strlen(prefix) : 0) < limit)
                continue;
        } else if (chunk >= end && n) {
            break;
        }

        /* Cut at the last boundary outside a code block. If the block itself is
         * longer than the limit, cut mid-block and close/reopen the fence so
         * each part parses standalone. */
        const char *cut = done ? end
                        : (safe && safe > chunk) ? safe : cur;
        int reopen = !done && fence_line && cut == cur;

        mdv2_buf t = {0};
        if (prefix) { mdv2_puts(&t, prefix); free(prefix); prefix = NULL; }
        mdv2_add(&t, chunk, (size_t)(cut - chunk));
        if (reopen) {
            if (t.n && t.p[t.n - 1] != '\n') mdv2_putc(&t, '\n');
            mdv2_puts(&t, "```");
            mdv2_buf p = {0};
            mdv2_add(&p, fence_line, fence_n);
            mdv2_putc(&p, '\n');
            prefix = p.p;
        }
        if (t.n) {
            if (n == cap) {
                cap *= 2;
                char **q = (char **)realloc(parts, cap * sizeof *parts);
                if (!q) { free(t.p); break; }
                parts = q;
            }
            parts[n++] = t.p;
        } else {
            free(t.p);
        }

        chunk = cut;
        while (chunk < end && *chunk == '\n') chunk++;
        if (cur < chunk) cur = chunk;
        safe = NULL;
        if (done || chunk >= end) break;
    }

    free(prefix);
    if (n == 0) parts[n++] = mdv2_dup(src, (size_t)(end - src));
    *count = n;
    return parts;
}

/* ---- messages ----------------------------------------------------------- */

typedef struct { char **v; size_t n, cap; } mdv2_list;

static void mdv2_list_add(mdv2_list *l, char *s) {
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 4;
        char **q = (char **)realloc(l->v, cap * sizeof *q);
        if (!q) { free(s); return; }
        l->v = q; l->cap = cap;
    }
    l->v[l->n++] = s;
}

/* Last-resort cut of already-converted text: never split a UTF-8 sequence or
 * strand a backslash from the character it escapes. */
static void mdv2_trunc(char *s, size_t max) {
    if (strlen(s) <= max) return;
    size_t i = max;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    size_t bs = 0;
    while (bs < i && s[i - 1 - bs] == '\\') bs++;
    if (bs % 2) i--;
    s[i] = '\0';
}

static void mdv2_emit(mdv2_list *l, const char *src, size_t src_limit,
                      size_t max_len, int depth) {
    size_t np = 0;
    char **parts = mdv2_split(src, src_limit, &np);
    if (!parts) return;
    for (size_t i = 0; i < np; i++) {
        if (!parts[i]) continue;
        char *out = mdv2_convert(parts[i]);
        if (!out) { free(parts[i]); continue; }
        size_t len = strlen(out);
        if (len <= max_len) {
            mdv2_list_add(l, out);
        } else if (depth < 6 && strlen(parts[i]) > 128) {
            free(out);                       /* escaping overshot: split finer */
            mdv2_emit(l, parts[i], strlen(parts[i]) / 2, max_len, depth + 1);
        } else {
            mdv2_trunc(out, max_len);
            mdv2_list_add(l, out);
        }
        free(parts[i]);
    }
    free(parts);
}

static char **mdv2_messages(const char *src, size_t max_len, size_t *count) {
    mdv2_list l = {0};
    if (max_len < 128) max_len = 128;
    mdv2_emit(&l, src ? src : "", max_len, max_len, 0);
    if (l.n == 0) mdv2_list_add(&l, mdv2_dup("", 0));
    *count = l.n;
    return l.v;
}

#endif /* MDV2_H */
