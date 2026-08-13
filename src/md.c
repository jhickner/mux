#include "md.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

/* A run of text carrying one style, produced by the inline pass. Style 0 means
 * "unstyled"; other values are ui_role + 1 so the array can stay zero-filled. */
struct styled {
    char       *text;
    signed char *style;
    size_t      len, cap;
};

static void styled_init(struct styled *s)
{
    s->cap = 128;
    s->len = 0;
    s->text = malloc(s->cap);
    s->style = calloc(s->cap, 1);
}

static void styled_free(struct styled *s)
{
    free(s->text);
    free(s->style);
}

static void styled_push(struct styled *s, const char *bytes, size_t n, int role)
{
    if (!s->text || !s->style)
        return;
    if (s->len + n + 1 > s->cap) {
        size_t want = (s->len + n + 1) * 2;
        char *t = realloc(s->text, want);
        signed char *y = realloc(s->style, want);
        if (!t || !y) {
            free(t ? t : s->text);
            free(y ? y : s->style);
            s->text = NULL;
            s->style = NULL;
            return;
        }
        s->text = t;
        s->style = y;
        s->cap = want;
    }
    memcpy(s->text + s->len, bytes, n);
    memset(s->style + s->len, role, n);
    s->len += n;
    s->text[s->len] = '\0';
}

static int is_url_start(const char *p)
{
    return strncmp(p, "http://", 7) == 0 || strncmp(p, "https://", 8) == 0;
}

/* Find the closing delimiter `delim` (of length dlen) after `p`, or NULL. The
 * span must be non-empty and must not span a line break. */
static const char *find_close(const char *p, const char *delim, size_t dlen)
{
    for (const char *q = p; *q && *q != '\n'; q++)
        if (strncmp(q, delim, dlen) == 0)
            return q > p ? q : NULL;
    return NULL;
}

/* Parse inline spans into `out`, stripping the markers. */
static void inline_scan(const char *p, struct styled *out)
{
    while (*p) {
        if (*p == '`') {
            const char *end = find_close(p + 1, "`", 1);
            if (end) {
                styled_push(out, p + 1, (size_t)(end - p - 1), UI_CODE + 1);
                p = end + 1;
                continue;
            }
        }
        if (p[0] == '*' && p[1] == '*') {
            const char *end = find_close(p + 2, "**", 2);
            if (end) {
                styled_push(out, p + 2, (size_t)(end - p - 2), UI_BOLD + 1);
                p = end + 2;
                continue;
            }
        }
        if (p[0] == '*' && p[1] != ' ') {
            const char *end = find_close(p + 1, "*", 1);
            if (end) {
                styled_push(out, p + 1, (size_t)(end - p - 1), UI_ITALIC + 1);
                p = end + 1;
                continue;
            }
        }
        /* [label](url) keeps the label and drops the target. */
        if (*p == '[') {
            const char *close = find_close(p + 1, "]", 1);
            if (close && close[1] == '(') {
                const char *paren = find_close(close + 2, ")", 1);
                if (paren) {
                    styled_push(out, p + 1, (size_t)(close - p - 1), UI_LINK + 1);
                    p = paren + 1;
                    continue;
                }
            }
        }
        if (is_url_start(p)) {
            const char *end = p;
            while (*end && !isspace((unsigned char)*end) && *end != ')' && *end != '>')
                end++;
            styled_push(out, p, (size_t)(end - p), UI_LINK + 1);
            p = end;
            continue;
        }
        styled_push(out, p, 1, 0);
        p++;
    }
}

static void pad(int n)
{
    for (int i = 0; i < n; i++)
        ui_put(" ");
}

/* Emit a styled buffer wrapped to `width` cells, indenting the first row by
 * `first_indent` and the rest by `indent`. Style runs are closed at each row
 * break so no attribute spans a physical line. */
static void emit_styled(const struct styled *s, int first_indent, int indent)
{
    if (!s->text)
        return;
    int columns = ui_columns();
    const char *p = s->text;
    int row_indent = first_indent;

    while (*p || p == s->text) {
        int budget = columns - row_indent;
        if (budget < 8)
            budget = 8;
        size_t skip = 0;
        size_t row = *p ? ui_wrap_row(p, (size_t)budget, &skip) : 0;

        pad(row_indent);
        size_t base = (size_t)(p - s->text);
        /* -1 rather than 0, so the unstyled run also opens (as UI_BODY). */
        int open = -1;
        for (size_t i = 0; i < row; i++) {
            signed char style = s->style[base + i];
            if (style != open) {
                ui_esc(ui_style(UI_RESET));
                ui_esc(ui_style(style ? (enum ui_role)(style - 1) : UI_BODY));
                open = style;
            }
            ui_putn(p + i, 1);
        }
        if (open >= 0)
            ui_esc(ui_style(UI_RESET));
        ui_put("\n");

        p += row + skip;
        row_indent = indent;
        if (!*p)
            break;
    }
}

static void render_paragraph(const char *text, int first_indent, int indent)
{
    struct styled s;
    styled_init(&s);
    inline_scan(text, &s);
    emit_styled(&s, first_indent, indent);
    styled_free(&s);
}

/* Leading spaces, counting a tab as four. */
static int leading_indent(const char *line, const char **body)
{
    int n = 0;
    while (*line == ' ' || *line == '\t') {
        n += (*line == '\t') ? 4 : 1;
        line++;
    }
    *body = line;
    return n;
}

static int is_bullet(const char *p, const char **rest)
{
    if ((p[0] == '-' || p[0] == '*' || p[0] == '+') && p[1] == ' ') {
        *rest = p + 2;
        return 1;
    }
    return 0;
}

/* "12. " style ordered marker; *marker_len receives its byte length. */
static int is_ordered(const char *p, size_t *marker_len)
{
    const char *q = p;
    while (isdigit((unsigned char)*q))
        q++;
    if (q == p || (*q != '.' && *q != ')') || q[1] != ' ')
        return 0;
    *marker_len = (size_t)(q - p) + 2;
    return 1;
}

static int is_rule(const char *p)
{
    if (*p != '-' && *p != '*' && *p != '_')
        return 0;
    char c = *p;
    int n = 0;
    while (*p == c || *p == ' ') {
        if (*p == c)
            n++;
        p++;
    }
    return *p == '\0' && n >= 3;
}

/* Copy one line out of `text`, advancing *text past its newline. */
static char *take_line(const char **text)
{
    const char *start = *text;
    const char *nl = strchr(start, '\n');
    size_t n = nl ? (size_t)(nl - start) : strlen(start);
    char *line = malloc(n + 1);
    if (!line)
        return NULL;
    memcpy(line, start, n);
    line[n] = '\0';
    /* Trailing carriage returns and spaces are noise in a terminal. */
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' '))
        line[--n] = '\0';
    *text = nl ? nl + 1 : start + strlen(start);
    return line;
}

/* Emit a line from an ```ansi fence, turning the textual escape forms \e, \033
 * and \x1b into a real ESC so the model can drive SGR directly (a literal 0x1b
 * does not survive the transport). \\ is an escaped backslash; anything else
 * passes through as written. */
static void render_ansi_line(const char *line, int indent)
{
    pad(indent);
    for (const char *p = line; *p; ) {
        if (p[0] != '\\') {
            ui_putn(p, 1);
            p++;
            continue;
        }
        if (p[1] == 'e') {
            ui_putn("\x1b", 1);
            p += 2;
        } else if (p[1] == 'x' && (p[2] == '1') && (p[3] == 'b' || p[3] == 'B')) {
            ui_putn("\x1b", 1);
            p += 4;
        } else if (p[1] == '0' && p[2] == '3' && p[3] == '3') {
            ui_putn("\x1b", 1);
            p += 4;
        } else if (p[1] == '\\') {
            ui_putn("\\", 1);
            p += 2;
        } else {
            ui_putn(p, 1);
            p++;
        }
    }
    /* Close unconditionally so an unterminated SGR cannot leak into the prompt. */
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static void render_code_line(const char *line, int indent)
{
    pad(indent);
    ui_esc(ui_style(UI_CODE));
    ui_put(line);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

void md_render(const char *text, int indent)
{
    int in_code = 0;
    int code_ansi = 0;
    int blank_pending = 0;
    int wrote_any = 0;

    while (*text) {
        char *line = take_line(&text);
        if (!line)
            return;

        const char *body;
        int lead = leading_indent(line, &body);

        if (strncmp(body, "```", 3) == 0 || strncmp(body, "~~~", 3) == 0) {
            in_code = !in_code;
            code_ansi = in_code && strncmp(body + 3, "ansi", 4) == 0;
            free(line);
            continue;
        }
        if (in_code) {
            if (blank_pending && wrote_any)
                ui_put("\n");
            blank_pending = 0;
            if (code_ansi)
                render_ansi_line(line, indent + 2);
            else
                render_code_line(line, indent + 2);
            wrote_any = 1;
            free(line);
            continue;
        }

        if (!*body) {
            blank_pending = wrote_any;
            free(line);
            continue;
        }
        if (blank_pending) {
            ui_put("\n");
            blank_pending = 0;
        }

        if (is_rule(body)) {
            int width = ui_columns() - indent;
            pad(indent);
            ui_esc(ui_style(UI_DIM));
            for (int i = 0; i < width && i < 60; i++)
                ui_put("\xe2\x94\x80"); /* ─ */
            ui_esc(ui_style(UI_RESET));
            ui_put("\n");
        } else if (*body == '#') {
            const char *h = body;
            while (*h == '#')
                h++;
            while (*h == ' ')
                h++;
            pad(indent);
            ui_esc(ui_style(UI_HEADING));
            ui_put(h);
            ui_esc(ui_style(UI_RESET));
            ui_put("\n");
        } else if (*body == '>') {
            const char *q = body + 1;
            if (*q == ' ')
                q++;
            pad(indent);
            ui_esc(ui_style(UI_DIM));
            ui_put("\xe2\x94\x82 "); /* │ */
            ui_esc(ui_style(UI_RESET));
            render_paragraph(q, 0, indent + 2);
        } else {
            const char *rest;
            size_t marker = 0;
            int item_indent = indent + lead;
            if (is_bullet(body, &rest)) {
                pad(item_indent);
                ui_esc(ui_style(UI_CHROME));
                ui_put("\xe2\x80\xa2 "); /* • */
                ui_esc(ui_style(UI_RESET));
                render_paragraph(rest, 0, item_indent + 2);
            } else if (is_ordered(body, &marker)) {
                pad(item_indent);
                ui_esc(ui_style(UI_CHROME));
                ui_putn(body, marker - 1);
                ui_esc(ui_style(UI_RESET));
                ui_put(" ");
                render_paragraph(body + marker, 0, item_indent + (int)marker);
            } else {
                render_paragraph(body, item_indent, item_indent);
            }
        }
        wrote_any = 1;
        free(line);
    }
}
