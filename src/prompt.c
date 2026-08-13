#include "prompt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tty.h"
#include "ui.h"

#define REPL_STYLE_NONE ((signed char)-1)

struct cell {
    uint32_t    cp;
    signed char style;
};

struct frame {
    struct cell *cells;
    int          rows, cols, cap;
    int          cursor_x, cursor_y;
    int          have_cursor;
};

struct prompt {
    Repl         repl;
    struct frame frame;
    int          painted_rows; /* rows the last paint occupies, 0 when clean */
    int          cursor_row;   /* where the terminal cursor sits within them */
    char        *history_path;
};

/* ---------- history ---------- */

static void history_append(struct prompt *p, const char *line)
{
    if (!p->history_path || !*line)
        return;
    FILE *f = fopen(p->history_path, "a");
    if (!f)
        return;
    /* Newlines would split one entry into several on reload. */
    for (const char *q = line; *q; q++)
        fputc(*q == '\n' ? ' ' : *q, f);
    fputc('\n', f);
    fclose(f);
}

void prompt_history_open(struct prompt *p, const char *path)
{
    free(p->history_path);
    p->history_path = strdup(path);

    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        if (line[n - 1] == '\n')
            line[n - 1] = '\0';
        if (*line)
            repl_history_add(&p->repl, line);
    }
    free(line);
    fclose(f);
}

/* ---------- frame buffer ---------- */

static void frame_size(struct frame *f, int rows, int cols)
{
    int need = rows * cols;
    if (need > f->cap) {
        struct cell *grown = realloc(f->cells, (size_t)need * sizeof *grown);
        if (!grown)
            return;
        f->cells = grown;
        f->cap = need;
    }
    f->rows = rows;
    f->cols = cols;
    for (int i = 0; i < need; i++) {
        f->cells[i].cp = ' ';
        f->cells[i].style = REPL_STYLE_NONE;
    }
    f->have_cursor = 0;
    f->cursor_x = f->cursor_y = 0;
}

static void draw_cell(void *ctx, int x, int y, uint32_t cp, ReplStyle style)
{
    struct frame *f = ctx;
    if (x < 0 || y < 0 || x >= f->cols || y >= f->rows)
        return;
    if (style == REPL_STYLE_CURSOR) {
        f->cursor_x = x;
        f->cursor_y = y;
        f->have_cursor = 1;
    }
    struct cell *c = &f->cells[y * f->cols + x];
    c->cp = cp;
    c->style = (signed char)style;
}

static const char *style_open(ReplStyle style)
{
    switch (style) {
    case REPL_STYLE_PROMPT:   return ui_style(UI_CHROME);
    case REPL_STYLE_TYPED:    return ui_style(UI_TEXT);
    case REPL_STYLE_DIM:      return ui_style(UI_DIM);
    case REPL_STYLE_CURSOR:   return ui_style(UI_ACCENT);
    case REPL_STYLE_SELECTED: return ui_style(UI_ACCENT);
    }
    return "";
}

static void put_codepoint(uint32_t cp)
{
    char buf[4];
    int n;
    if (cp < 0x80) {
        buf[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    fwrite(buf, 1, (size_t)n, stdout);
}

/* Trailing unstyled blanks need no output; a highlighted row keeps its run so
 * the selection reads as a filled bar. */
static int row_extent(const struct frame *f, int y)
{
    int last = -1;
    for (int x = 0; x < f->cols; x++) {
        const struct cell *c = &f->cells[y * f->cols + x];
        int blank = (c->cp == ' ' &&
                     (c->style == REPL_STYLE_NONE || c->style == REPL_STYLE_TYPED ||
                      c->style == REPL_STYLE_DIM));
        if (!blank)
            last = x;
    }
    return last + 1;
}

/* Move the terminal cursor to the top-left of the painted block. */
static void goto_origin(struct prompt *p)
{
    if (p->cursor_row > 0)
        printf("\x1b[%dA", p->cursor_row);
    fputs("\r", stdout);
    p->cursor_row = 0;
}

static void erase_block(struct prompt *p)
{
    if (p->painted_rows == 0)
        return;
    goto_origin(p);
    fputs("\x1b[J", stdout);
    fflush(stdout);
    p->painted_rows = 0;
}

/* The caret repl.h draws past the end of a row is a placeholder for the real
 * terminal cursor, so it must not be printed. */
static int caret_is_synthetic(const Repl *r)
{
    return r->cursor >= r->len || r->buf[r->cursor] == '\n';
}

static void repaint(struct prompt *p)
{
    int cols = ui_columns();
    int input_rows = repl_input_rows(&p->repl, cols);
    int rows = input_rows + repl_dropdown_rows(&p->repl);
    if (rows < 1)
        rows = 1;

    frame_size(&p->frame, rows, cols);
    if (!p->frame.cells)
        return;
    repl_render(&p->repl, 0, 0, cols, true, draw_cell, &p->frame);

    int synthetic = caret_is_synthetic(&p->repl);

    goto_origin(p);
    fputs("\x1b[?25l", stdout); /* hide while the block redraws */

    for (int y = 0; y < rows; y++) {
        fputs("\x1b[K", stdout);
        int extent = row_extent(&p->frame, y);
        signed char open = REPL_STYLE_NONE;
        for (int x = 0; x < extent; x++) {
            struct cell *c = &p->frame.cells[y * p->frame.cols + x];
            uint32_t cp = c->cp;
            /* The line being edited wears a caret; it becomes the "▌" block only
             * once it is submitted into the history above. repl.h prefixes the
             * first row with "> " and continuation rows with two spaces. */
            if (c->style == REPL_STYLE_PROMPT && x == 0 && cp == '>')
                cp = 0x276F; /* ❯ */
            if (c->style == REPL_STYLE_CURSOR && cp == '_' && synthetic &&
                p->frame.cursor_x == x && p->frame.cursor_y == y)
                cp = ' ';
            if (c->style != open) {
                fputs(ui_style(UI_RESET), stdout);
                if (c->style != REPL_STYLE_NONE)
                    fputs(style_open((ReplStyle)c->style), stdout);
                open = c->style;
            }
            put_codepoint(cp);
        }
        if (open != REPL_STYLE_NONE)
            fputs(ui_style(UI_RESET), stdout);
        if (y + 1 < rows)
            fputs("\r\n", stdout);
    }
    /* Clear anything the previous, taller block left below. */
    fputs("\x1b[J", stdout);

    int target_row = p->frame.have_cursor ? p->frame.cursor_y : rows - 1;
    int target_col = p->frame.have_cursor ? p->frame.cursor_x : 0;
    int up = (rows - 1) - target_row;
    if (up > 0)
        printf("\x1b[%dA", up);
    fputs("\r", stdout);
    if (target_col > 0)
        printf("\x1b[%dC", target_col);
    fputs("\x1b[?25h", stdout);
    fflush(stdout);

    p->painted_rows = rows;
    p->cursor_row = target_row;
}

/* ---------- the submitted block left in scrollback ---------- */

void prompt_echo_message(const char *text)
{
    int cols = ui_columns();
    size_t budget = (size_t)(cols - 2 > 4 ? cols - 2 : 4);
    const char *p = text;
    int first = 1;

    while (*p || first) {
        size_t skip = 0;
        size_t row = *p ? ui_wrap_row(p, budget, &skip) : 0;
        ui_esc(ui_style(UI_ACCENT));
        ui_put(UI_BAR);
        ui_esc(ui_style(UI_TEXT));
        ui_put(" ");
        ui_putn(p, row);
        ui_esc(ui_style(UI_RESET));
        ui_put("\n");
        p += row + skip;
        first = 0;
    }
    ui_put("\n");
    ui_flush();
}

/* ---------- lifecycle ---------- */

struct prompt *prompt_new(const ReplCommand *commands, int command_count)
{
    struct prompt *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    repl_init(&p->repl, commands, command_count);
    return p;
}

void prompt_free(struct prompt *p)
{
    if (!p)
        return;
    repl_free(&p->repl);
    free(p->frame.cells);
    free(p->history_path);
    free(p);
}

/* ---------- input loop ---------- */

static int feed(struct prompt *p, ReplKey key, uint32_t cp, const char *text)
{
    ReplEvent ev = {.key = key, .codepoint = cp, .text = text};
    return repl_handle_input(&p->repl, &ev);
}

/* Forward-delete, which repl.h has no key for: step right, then delete back. */
static void delete_forward(struct prompt *p)
{
    if (p->repl.dropdown_open || p->repl.cursor >= p->repl.len)
        return;
    feed(p, REPL_KEY_RIGHT, 0, NULL);
    feed(p, REPL_KEY_BACKSPACE, 0, NULL);
}

char *prompt_read(struct prompt *p)
{
    repl_reset(&p->repl);
    p->painted_rows = 0;
    p->cursor_row = 0;
    repaint(p);

    for (;;) {
        tty_event ev;
        if (!tty_read(&ev, -1))
            continue;

        switch (ev.key) {
        case TK_EOF:
            erase_block(p);
            return NULL;

        case TK_RESIZE:
            repaint(p);
            continue;

        case TK_TEXT:
            if (ev.text) {
                repl_insert_text(&p->repl, ev.text);
                free(ev.text);
            }
            repaint(p);
            continue;

        case TK_CHAR:
            if (ev.cp == 4) { /* Ctrl-D */
                if (p->repl.len == 0) {
                    erase_block(p);
                    return NULL;
                }
                delete_forward(p);
                repaint(p);
                continue;
            }
            if (ev.cp == 12) { /* Ctrl-L */
                p->painted_rows = 0;
                p->cursor_row = 0;
                fputs("\x1b[2J\x1b[H", stdout);
                fflush(stdout);
                repaint(p);
                continue;
            }
            feed(p, REPL_KEY_CHAR, ev.cp, NULL);
            repaint(p);
            continue;

        case TK_TAB:
            /* repl.h accepts a candidate or an inline suggestion with Right. */
            if (p->repl.dropdown_open || repl_suggestion(&p->repl))
                feed(p, REPL_KEY_RIGHT, 0, NULL);
            repaint(p);
            continue;

        case TK_DELETE:
            delete_forward(p);
            repaint(p);
            continue;

        case TK_ENTER: {
            ReplResult r = feed(p, REPL_KEY_ENTER, 0, NULL);
            if (r != REPL_SUBMIT) {
                repaint(p);
                continue;
            }
            const char *line = repl_line(&p->repl);
            if (!line || !*line) {
                repaint(p);
                continue;
            }
            char *out = strdup(line);
            repl_history_add(&p->repl, line);
            history_append(p, line);
            erase_block(p);
            if (out)
                prompt_echo_message(out);
            return out;
        }

        default: {
            static const ReplKey MAP[] = {
                [TK_NEWLINE] = REPL_KEY_NEWLINE,   [TK_BACKSPACE] = REPL_KEY_BACKSPACE,
                [TK_ESCAPE] = REPL_KEY_ESCAPE,     [TK_LEFT] = REPL_KEY_LEFT,
                [TK_RIGHT] = REPL_KEY_RIGHT,       [TK_UP] = REPL_KEY_UP,
                [TK_DOWN] = REPL_KEY_DOWN,         [TK_WORD_LEFT] = REPL_KEY_WORD_LEFT,
                [TK_WORD_RIGHT] = REPL_KEY_WORD_RIGHT,
            };
            if (ev.key == TK_HOME)
                feed(p, REPL_KEY_CHAR, 1, NULL); /* ctrl-a */
            else if (ev.key == TK_END)
                feed(p, REPL_KEY_CHAR, 5, NULL); /* ctrl-e */
            else if ((size_t)ev.key < sizeof MAP / sizeof *MAP)
                feed(p, MAP[ev.key], 0, NULL);
            repaint(p);
            continue;
        }
        }
    }
}
