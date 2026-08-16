#include "md.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"
#include "ui.h"

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

static const char *find_close(const char *p, const char *delim, size_t dlen)
{
    for (const char *q = p; *q && *q != '\n'; q++)
        if (strncmp(q, delim, dlen) == 0)
            return q > p ? q : NULL;
    return NULL;
}

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

static char *take_line(const char **text);

#define TABLE_MAX_COLS 12
#define TABLE_MIN_COL  6

enum align { ALIGN_LEFT, ALIGN_RIGHT, ALIGN_CENTER };

struct trow {
    struct styled cell[TABLE_MAX_COLS];
    int           ncells;
};

static int split_row(const char *line, char **out, int max)
{
    const char *p = line;
    while (*p == ' ')
        p++;
    if (*p == '|')
        p++;

    char buf[2048];
    size_t b = 0;
    int n = 0;

    for (;; p++) {
        if (*p == '\\' && p[1] == '|') {
            if (b + 1 < sizeof buf)
                buf[b++] = '|';
            p++;
            continue;
        }
        if (*p != '|' && *p != '\0') {
            if (b + 1 < sizeof buf)
                buf[b++] = *p;
            continue;
        }

        size_t start = 0, end = b;
        while (start < end && buf[start] == ' ')
            start++;
        while (end > start && buf[end - 1] == ' ')
            end--;
        if (n < max) {
            char *cell = malloc(end - start + 1);
            if (!cell)
                return n;
            memcpy(cell, buf + start, end - start);
            cell[end - start] = '\0';
            out[n++] = cell;
        }
        b = 0;

        if (*p == '\0')
            break;

        const char *q = p + 1;
        while (*q == ' ')
            q++;
        if (*q == '\0')
            break;
    }
    return n;
}

static int is_delim_cell(const char *s, enum align *out)
{
    const char *p = s;
    int left = 0, right = 0, dashes = 0;
    if (*p == ':') {
        left = 1;
        p++;
    }
    while (*p == '-') {
        dashes++;
        p++;
    }
    if (*p == ':') {
        right = 1;
        p++;
    }
    if (*p || dashes == 0)
        return 0;
    *out = left && right ? ALIGN_CENTER : right ? ALIGN_RIGHT : ALIGN_LEFT;
    return 1;
}

static int is_delim_row(const char *line, enum align *align, int max)
{
    if (!strchr(line, '-') || !strchr(line, '|'))
        return 0;

    char *cells[TABLE_MAX_COLS];
    int n = split_row(line, cells, max);
    int ok = n > 0;
    for (int i = 0; i < n; i++) {
        enum align a = ALIGN_LEFT;
        if (!is_delim_cell(cells[i], &a))
            ok = 0;
        else if (align)
            align[i] = a;
        free(cells[i]);
    }
    return ok;
}

static int peek_is_delim(const char *text, enum align *align, int max)
{
    const char *nl = strchr(text, '\n');
    size_t n = nl ? (size_t)(nl - text) : strlen(text);
    if (n >= 512)
        return 0;
    char line[512];
    memcpy(line, text, n);
    line[n] = '\0';
    return is_delim_row(line, align, max);
}

static void row_init(struct trow *r, const char *line, int ncols)
{
    char *cells[TABLE_MAX_COLS];
    int n = split_row(line, cells, ncols);
    for (int i = 0; i < ncols; i++) {
        styled_init(&r->cell[i]);
        if (i < n) {
            inline_scan(cells[i], &r->cell[i]);
            free(cells[i]);
        }
    }

    for (int i = ncols; i < n; i++)
        free(cells[i]);
    r->ncells = ncols;
}

static void row_free(struct trow *r)
{
    for (int i = 0; i < r->ncells; i++)
        styled_free(&r->cell[i]);
}

static void emit_slice(const struct styled *s, size_t from, size_t len, enum ui_role base)
{
    int open = -1;
    for (size_t i = 0; i < len; i++) {
        signed char style = s->style[from + i];
        if (style != open) {
            ui_esc(ui_style(UI_RESET));
            ui_esc(ui_style(style ? (enum ui_role)(style - 1) : base));
            open = style;
        }
        ui_putn(s->text + from + i, 1);
    }
    if (open >= 0)
        ui_esc(ui_style(UI_RESET));
}

static void rule_row(const int *width, int ncols, int indent)
{
    pad(indent);
    ui_esc(ui_style(UI_CHROME));
    for (int c = 0; c < ncols; c++) {
        if (c)
            ui_put("\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80");
        for (int i = 0; i < width[c]; i++)
            ui_put("\xe2\x94\x80");
    }
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static void render_row(const struct trow *r, const int *width, const enum align *align, int ncols,
                       int indent, int header)
{
    size_t off[TABLE_MAX_COLS] = {0};
    int more = 1;

    while (more) {
        more = 0;
        pad(indent);
        for (int c = 0; c < ncols; c++) {
            const struct styled *s = &r->cell[c];
            size_t take = 0, skip = 0;
            if (s->text && off[c] < s->len) {
                take = ui_wrap_row(s->text + off[c], (size_t)width[c], &skip);
                if (take == 0 && skip == 0)
                    take = s->len - off[c];
            }

            int used = take ? (int)ui_cells_n(s->text + off[c], take) : 0;
            int slack = width[c] - used;
            if (slack < 0)
                slack = 0;
            int before = align[c] == ALIGN_RIGHT    ? slack
                         : align[c] == ALIGN_CENTER ? slack / 2
                                                    : 0;

            int last = (c == ncols - 1);
            int blank = (take == 0);

            if (c) {
                ui_esc(ui_style(UI_CHROME));
                ui_put(" \xe2\x94\x82");
                ui_esc(ui_style(UI_RESET));
                if (!(last && blank))
                    ui_put(" ");
            }
            if (!(last && blank)) {
                pad(before);
                if (take)
                    emit_slice(s, off[c], take, header ? UI_BOLD : UI_BODY);
                if (!last)
                    pad(slack - before);
            }

            off[c] += take + skip;
            if (s->text && off[c] < s->len)
                more = 1;
        }
        ui_put("\n");
    }
}

static void fit_widths(int *width, int ncols, int indent)
{
    int avail = ui_columns() - indent - 3 * (ncols - 1);
    if (avail < ncols * TABLE_MIN_COL)
        avail = ncols * TABLE_MIN_COL;

    int total = 0;
    for (int c = 0; c < ncols; c++)
        total += width[c];

    while (total > avail) {
        int widest = 0;
        for (int c = 1; c < ncols; c++)
            if (width[c] > width[widest])
                widest = c;
        if (width[widest] <= TABLE_MIN_COL)
            break;
        width[widest]--;
        total--;
    }
}

static void render_table(const char *first, const char **text, int indent)
{
    enum align align[TABLE_MAX_COLS];
    for (int i = 0; i < TABLE_MAX_COLS; i++)
        align[i] = ALIGN_LEFT;

    char *head[TABLE_MAX_COLS];
    int ncols = split_row(first, head, TABLE_MAX_COLS);
    for (int i = 0; i < ncols; i++)
        free(head[i]);
    if (ncols == 0)
        return;

    char *delim = take_line(text);
    if (!delim)
        return;
    is_delim_row(delim, align, ncols);
    free(delim);

    struct trow *rows = NULL;
    int count = 0, cap = 0;

    struct trow header;
    row_init(&header, first, ncols);

    while (**text) {
        const char *nl = strchr(*text, '\n');
        size_t n = nl ? (size_t)(nl - *text) : strlen(*text);
        if (n == 0 || !memchr(*text, '|', n))
            break;

        char *line = take_line(text);
        if (!line)
            break;
        if (count == cap) {
            int want = cap ? cap * 2 : 8;
            struct trow *grown = realloc(rows, (size_t)want * sizeof *grown);
            if (!grown) {
                free(line);
                break;
            }
            rows = grown;
            cap = want;
        }
        row_init(&rows[count++], line, ncols);
        free(line);
    }

    int width[TABLE_MAX_COLS];
    for (int c = 0; c < ncols; c++) {
        width[c] = (int)ui_cells_n(header.cell[c].text, header.cell[c].len);
        for (int r = 0; r < count; r++) {
            int w = (int)ui_cells_n(rows[r].cell[c].text, rows[r].cell[c].len);
            if (w > width[c])
                width[c] = w;
        }
        if (width[c] < 1)
            width[c] = 1;
    }
    fit_widths(width, ncols, indent);

    render_row(&header, width, align, ncols, indent, 1);
    rule_row(width, ncols, indent);
    for (int r = 0; r < count; r++)
        render_row(&rows[r], width, align, ncols, indent, 0);

    row_free(&header);
    for (int r = 0; r < count; r++)
        row_free(&rows[r]);
    free(rows);
}

static void render_paragraph(const char *text, int first_indent, int indent)
{
    struct styled s;
    styled_init(&s);
    inline_scan(text, &s);
    emit_styled(&s, first_indent, indent);
    styled_free(&s);
}

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

    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' '))
        line[--n] = '\0';
    *text = nl ? nl + 1 : start + strlen(start);
    return line;
}

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

    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static char *image_line(const char *body)
{
    if (body[0] != '!' || body[1] != '[')
        return NULL;
    const char *close = strchr(body + 2, ']');
    if (!close || close[1] != '(')
        return NULL;
    const char *open = close + 2;
    const char *end = strchr(open, ')');
    if (!end || end[1] != '\0' || end == open || is_url_start(open))
        return NULL;

    size_t n = (size_t)(end - open);
    char *path = malloc(n + 1);
    if (!path)
        return NULL;
    memcpy(path, open, n);
    path[n] = '\0';
    return path;
}

static void render_image(const char *path, int indent)
{
    if (img_show(path, indent))
        return;
    pad(indent);
    ui_esc(ui_style(UI_DIM));
    ui_put("[image] ");
    ui_put(path);
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

        char *img = image_line(body);
        if (img) {
            render_image(img, indent);
            free(img);
            wrote_any = 1;
            free(line);
            continue;
        }

        if (strchr(body, '|') && peek_is_delim(text, NULL, TABLE_MAX_COLS)) {
            render_table(body, &text, indent);
            wrote_any = 1;
            free(line);
            continue;
        }

        if (is_rule(body)) {
            int width = ui_columns() - indent;
            pad(indent);
            ui_esc(ui_style(UI_DIM));
            for (int i = 0; i < width && i < 60; i++)
                ui_put("\xe2\x94\x80");
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
            ui_put("\xe2\x94\x82 ");
            ui_esc(ui_style(UI_RESET));
            render_paragraph(q, 0, indent + 2);
        } else {
            const char *rest;
            size_t marker = 0;
            int item_indent = indent + lead;
            if (is_bullet(body, &rest)) {
                pad(item_indent);
                ui_esc(ui_style(UI_CHROME));
                ui_put("\xe2\x80\xa2 ");
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
