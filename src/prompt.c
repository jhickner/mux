#include "prompt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "files.h"
#include "paste.h"
#include "status.h"
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
    int          painted_cols; /* the width it was painted at */
    int          caret_row;    /* rows from the top of the block to the caret */
    int          caret_col;
    int          caret_frame_row; /* the caret's row within the frame */
    char        *history_path;
    prompt_live_fn live_command;
    void          *live_ud;
    char       **queued;       /* lines submitted while a turn was running */
    int          queued_count;
    int          queued_cap;
    char        *file_root;    /* project root for @-completion, or NULL */
    prompt_status_fn status;   /* the gauge in front of the caret, or NULL */
    void            *status_ud;
    int        (*idle_fd)(void *ud);   /* output arriving unprompted, or NULL */
    void       (*idle_render)(void *ud);
    void        *idle_ud;
    int          live_block;   /* the block on screen is the turn-time one */
    const char  *painted_head; /* the floating prompt the block on screen carries.
                                * status.c owns the text and only replaces it
                                * between turns, when no block is painted */
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
    ui_putn(buf, (size_t)n);
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

static size_t queued_budget(int cols)
{
    return (size_t)(cols - 2 > 4 ? cols - 2 : 4);
}

/* Rows a line of `cells` cells occupies at this width. */
static int rows_for(size_t cells, int cols)
{
    return cells ? (int)((cells - 1) / (size_t)cols) + 1 : 1;
}

/* The floating prompt carried above an idle editor: the message the last turn
 * answered, once its echo has scrolled off the top. The turn-time block sits
 * under the spinner, which floats it there itself. */
static const char *head_text(const struct prompt *p)
{
    return p->live_block ? NULL : status_sticky_offscreen();
}

/* Cells a "▌ " bar may fill. The floating prompt reserves one more for the
 * ellipsis a clipped last row may carry. */
static size_t sticky_budget(int cols)
{
    return (size_t)(cols - 3 > 4 ? cols - 3 : 4);
}

/* Rows one bar message occupies when wrapped at `budget` cells. `cap` > 0
 * stops after that many rows, matching the floating-prompt clip. */
static int wrapped_rows(const char *text, size_t budget, int cap)
{
    int rows = 0, first = 1;
    while (*text || first) {
        size_t skip = 0;
        size_t row = *text ? ui_wrap_row(text, budget, &skip) : 0;
        rows++;
        text += row + skip;
        first = 0;
        if (cap && rows >= cap)
            break;
    }
    return rows;
}

/* The same message as the terminal shows it now: each row it was painted on is
 * re-measured against the current width, since a resize rewraps them. `cap` is
 * the clip used at paint time; the last of those rows is one cell wider when
 * an ellipsis was appended. */
static int reflowed_rows(const char *text, size_t budget, int cols, int cap)
{
    int rows = 0, first = 1, painted = 0;
    while (*text || first) {
        size_t skip = 0;
        size_t row = *text ? ui_wrap_row(text, budget, &skip) : 0;
        int width = 2 + (int)ui_cells_n(text, row); /* the "▌ " prefix */
        text += row + skip;
        painted++;
        if (*text && cap && painted == cap)
            width++;
        rows += rows_for((size_t)width, cols);
        first = 0;
        if (cap && painted >= cap)
            break;
    }
    return rows;
}

/* Rows from the top of the painted block down to the caret, as the terminal
 * shows them now. A resize rewraps what is on screen, so any painted row that
 * no longer fits sits on more rows than it did when it was drawn; walking up by
 * the old count would land inside the block and leave its top rows stranded. */
static int caret_offset(const struct prompt *p, int cols)
{
    if (p->painted_rows == 0 || cols == p->painted_cols || p->painted_cols <= 0)
        return p->caret_row;

    int up = 0;
    size_t budget = queued_budget(p->painted_cols);
    if (p->painted_head)
        up += reflowed_rows(p->painted_head, sticky_budget(p->painted_cols),
                            cols, STICKY_LINES) + 1; /* and its blank */
    for (int i = 0; i < p->queued_count; i++)
        up += reflowed_rows(p->queued[i], budget, cols, 0);
    for (int y = 0; y < p->caret_frame_row && y < p->frame.rows; y++)
        up += rows_for((size_t)row_extent(&p->frame, y), cols);
    return up + p->caret_col / cols;
}

/* Move the terminal cursor to the top-left of the painted block. */
static void goto_origin(struct prompt *p)
{
    int up = caret_offset(p, ui_columns());
    if (up > 0) {
        char esc[32];
        snprintf(esc, sizeof esc, "\x1b[%dA", up);
        ui_esc(esc);
    }
    ui_esc("\r");
    p->caret_row = 0;
}

static void erase_block(struct prompt *p)
{
    if (p->painted_rows == 0)
        return;
    goto_origin(p);
    ui_esc("\x1b[J");
    fflush(stdout);
    p->painted_rows = 0;
    p->painted_head = NULL;
}

/* The caret repl.h draws past the end of a row is a placeholder for the real
 * terminal cursor, so it must not be printed. */
static int caret_is_synthetic(const Repl *r)
{
    return r->cursor >= r->len || r->buf[r->cursor] == '\n';
}

/* Rows above the editor — the floating prompt with its blank row, then the
 * queued messages — which the block height depends on. Must agree with
 * paint_above(). */
static int rows_above(const struct prompt *p, const char *head, int cols)
{
    int rows = head ? wrapped_rows(head, sticky_budget(cols), STICKY_LINES) + 1 : 0;
    size_t budget = queued_budget(cols);
    for (int i = 0; i < p->queued_count; i++)
        rows += wrapped_rows(p->queued[i], budget, 0);
    return rows;
}

/* One "▌ " message, wrapped the way the history echo wraps it so a message
 * reads the same before and after it runs. */
static void paint_bars(const char *text, size_t budget, enum ui_role role, int cap)
{
    int first = 1, painted = 0;
    while (*text || first) {
        size_t skip = 0;
        size_t row = *text ? ui_wrap_row(text, budget, &skip) : 0;
        ui_esc("\x1b[K");
        ui_esc(ui_style(role));
        ui_put(UI_BAR " ");
        ui_putn(text, row);
        text += row + skip;
        painted++;
        if (*text && cap && painted == cap)
            ui_put("…");
        ui_esc(ui_style(UI_RESET));
        ui_put("\n");
        first = 0;
        if (cap && painted >= cap)
            break;
    }
}

/* The floating prompt, then whatever was queued during the last turn: what is
 * waiting to run is dim, the message already answered wears the sticky color.
 * A long floating prompt is clipped to STICKY_LINES. */
static void paint_above(const struct prompt *p, const char *head, int cols)
{
    if (head) {
        paint_bars(head, sticky_budget(cols), UI_STICKY, STICKY_LINES);
        ui_esc("\x1b[K");
        ui_put("\n");
    }
    size_t budget = queued_budget(cols);
    for (int i = 0; i < p->queued_count; i++)
        paint_bars(p->queued[i], budget, UI_DIM, 0);
}

/* Cells the gauge reserves in front of the caret, including the space after it.
 * The editor is rendered inset by this much so its wrapped rows and its
 * dropdown line up under the caret rather than under the gauge. Yields the
 * whole width back rather than squeeze the editor into a narrow terminal. */
static int status_indent(const struct prompt *p, const char **text, int cols)
{
    *text = p->status ? p->status(p->status_ud) : NULL;
    if (!*text || !**text)
        return 0;
    int width = (int)strlen(*text) + 1;
    if (cols < 20 || width > cols / 4) {
        *text = NULL;
        return 0;
    }
    return width;
}

/* Draw the block from the cursor's row down, leaving the cursor at the end of
 * the last row. Reports the height and where the caret belongs within it. */
static void paint_block(struct prompt *p, int *rows_out, int *caret_row, int *caret_col)
{
    int cols = ui_columns();
    const char *status = NULL;
    int indent = status_indent(p, &status, cols);
    int input_rows = repl_input_rows(&p->repl, cols - indent);
    int rows = input_rows + repl_dropdown_rows(&p->repl);
    if (rows < 1)
        rows = 1;

    const char *head = head_text(p);
    int above = rows_above(p, head, cols);
    *rows_out = rows + above;
    *caret_row = above;
    *caret_col = 0;

    frame_size(&p->frame, rows, cols);
    if (!p->frame.cells) {
        *rows_out = 1;
        *caret_row = 0;
        p->painted_cols = cols;
        p->painted_head = NULL;
        p->caret_row = p->caret_col = p->caret_frame_row = 0;
        return;
    }
    repl_render(&p->repl, indent, 0, cols - indent, true, draw_cell, &p->frame);
    for (int x = 0; status && status[x]; x++)
        draw_cell(&p->frame, x, 0, (unsigned char)status[x], REPL_STYLE_DIM);

    int synthetic = caret_is_synthetic(&p->repl);

    *caret_row += p->frame.have_cursor ? p->frame.cursor_y : rows - 1;
    *caret_col = p->frame.have_cursor ? p->frame.cursor_x : 0;

    /* Kept so the next erase can re-measure the block against a width that may
     * have changed under it. */
    p->painted_cols = cols;
    p->painted_head = head;
    p->caret_row = *caret_row;
    p->caret_col = *caret_col;
    p->caret_frame_row = p->frame.have_cursor ? p->frame.cursor_y : rows - 1;

    paint_above(p, head, cols);

    for (int y = 0; y < rows; y++) {
        ui_esc("\x1b[K");
        int extent = row_extent(&p->frame, y);
        const char *open = "";
        for (int x = 0; x < extent; x++) {
            struct cell *c = &p->frame.cells[y * p->frame.cols + x];
            uint32_t cp = c->cp;
            const char *seq = c->style == REPL_STYLE_NONE
                                  ? "" : style_open((ReplStyle)c->style);
            /* The line being edited wears a caret; it becomes the "▌" block only
             * once it is submitted into the history above. repl.h prefixes the
             * first row with "> " and continuation rows with two spaces. */
            if (c->style == REPL_STYLE_PROMPT && x == indent && cp == '>') {
                cp = 0x276F; /* ❯ */
                seq = ui_style(UI_ACCENT);
            }
            if (c->style == REPL_STYLE_CURSOR && cp == '_' && synthetic &&
                p->frame.cursor_x == x && p->frame.cursor_y == y)
                cp = ' ';
            if (seq != open) {
                ui_esc(ui_style(UI_RESET));
                ui_esc(seq);
                open = seq;
            }
            put_codepoint(cp);
        }
        if (*open)
            ui_esc(ui_style(UI_RESET));
        if (y + 1 < rows)
            ui_put("\n");
    }
    /* Clear anything the previous, taller block left below. */
    ui_esc("\x1b[J");
}

static void repaint(struct prompt *p)
{
    int rows = 1, caret_row = 0, caret_col = 0;

    p->live_block = 0;
    /* Redrawn in place, so it scrolls nothing that outlives it. */
    ui_scroll_track(0);
    goto_origin(p);
    ui_esc("\x1b[?25l"); /* hide while the block redraws */
    paint_block(p, &rows, &caret_row, &caret_col);

    int up = (rows - 1) - caret_row;
    if (up > 0) {
        char esc[32];
        snprintf(esc, sizeof esc, "\x1b[%dA", up);
        ui_esc(esc);
    }
    ui_esc("\r");
    if (caret_col > 0) {
        char esc[32];
        snprintf(esc, sizeof esc, "\x1b[%dC", caret_col);
        ui_esc(esc);
    }
    ui_esc("\x1b[?25h");
    ui_scroll_track(1);
    fflush(stdout);

    p->painted_rows = rows;
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
    /* History stays available through up/down and Ctrl-R; only the inline ghost
     * text is off. */
    p->repl.suggest_off = true;
    return p;
}

void prompt_file_completion(struct prompt *p, const char *root)
{
    free(p->file_root);
    p->file_root = strdup(root);
    if (p->file_root)
        repl_set_completer(&p->repl, files_complete, p->file_root);
}

void prompt_free(struct prompt *p)
{
    if (!p)
        return;
    repl_free(&p->repl);
    files_forget();
    free(p->frame.cells);
    free(p->file_root);
    free(p->history_path);
    for (int i = 0; i < p->queued_count; i++)
        free(p->queued[i]);
    free(p->queued);
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

/* Ctrl-V pastes the clipboard. An image is saved to a file and the editor gets
 * its path, which is what the agent needs to open it; anything else inserts as
 * text, the same way the terminal's own bracketed paste arrives. */
static void paste_clipboard(struct prompt *p, int live)
{
    char path[1024];
    if (paste_image(path, sizeof path - 1)) {
        size_t n = strlen(path);
        path[n] = ' ';
        path[n + 1] = '\0';
        repl_insert_text(&p->repl, path);
        return;
    }

    char *text = paste_text();
    if (text) {
        repl_insert_text(&p->repl, text);
        free(text);
        return;
    }

    if (live) /* the turn owns the screen; a note would land inside its output */
        return;
    erase_block(p);
    ui_note("clipboard is empty");
    repaint(p);
}

/* A one-off path under $TMPDIR (or /tmp). Quoted later, so a quote in the
 * directory name would break the $EDITOR command; refuse rather than inject. */
static int editor_temp(char *out, size_t size)
{
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir)
        dir = "/tmp";
    if (strchr(dir, '\''))
        return 0;
    if ((size_t)snprintf(out, size, "%s/simple-agent-XXXXXX", dir) >= size)
        return 0;
    int fd = mkstemp(out);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

static char *read_whole(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    /* Editors almost always write a trailing newline the user did not type. */
    if (got && buf[got - 1] == '\n')
        buf[--got] = '\0';
    if (got && buf[got - 1] == '\r')
        buf[--got] = '\0';
    return buf;
}

/* Ctrl-G hands the current buffer to $VISUAL or $EDITOR (vi if neither is set)
 * and puts whatever the editor writes back into the line, including newlines. */
static void edit_in_editor(struct prompt *p, int live)
{
    if (p->repl.searching)
        feed(p, REPL_KEY_ENTER, 0, NULL);

    const char *editor = getenv("VISUAL");
    if (!editor || !*editor)
        editor = getenv("EDITOR");
    if (!editor || !*editor)
        editor = "vi";

    char path[256];
    if (!editor_temp(path, sizeof path)) {
        if (!live) {
            erase_block(p);
            ui_note("could not create a temp file for $EDITOR");
            repaint(p);
        }
        return;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        unlink(path);
        if (!live) {
            erase_block(p);
            ui_note("could not write %s", path);
            repaint(p);
        }
        return;
    }
    const char *line = repl_line(&p->repl);
    int wrote = 1;
    if (line && *line)
        wrote = fputs(line, f) >= 0;
    if (fclose(f) != 0)
        wrote = 0;
    if (!wrote) {
        unlink(path);
        if (!live) {
            erase_block(p);
            ui_note("could not write %s", path);
            repaint(p);
        }
        return;
    }

    char cmd[4096];
    if ((size_t)snprintf(cmd, sizeof cmd, "%s '%s'", editor, path) >= sizeof cmd) {
        unlink(path);
        if (!live) {
            erase_block(p);
            ui_note("$EDITOR command is too long");
            repaint(p);
        }
        return;
    }

    if (live)
        status_pause();
    else
        erase_block(p);
    ui_raw(0);
    tty_raw_end();

    int status = system(cmd);

    tty_raw_begin();
    ui_raw(1);
    ui_cursor_plain();

    int ok = status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (ok) {
        char *text = read_whole(path);
        if (text) {
            repl_reset(&p->repl);
            if (*text)
                repl_insert_text(&p->repl, text);
            free(text);
        } else {
            ok = 0;
        }
    }
    unlink(path);

    if (live)
        status_resume();
    else if (!ok)
        ui_note("$EDITOR failed");
}

enum key_result {
    KEY_OK,      /* the editor consumed it; redraw   */
    KEY_SUBMIT,  /* a complete line is ready to take */
    KEY_EOF,     /* Ctrl-D on an empty line, or EOF  */
    KEY_CANCEL,  /* Escape or Ctrl-C, live mode only */
};

/* Feed one terminal event to the editor. `live` marks the turn-time prompt,
 * where the screen belongs to the running turn: Ctrl-L is ignored there, and
 * Escape / Ctrl-C mean "interrupt the turn" rather than reaching the editor. */
static enum key_result feed_key(struct prompt *p, tty_event *ev, int live)
{
    switch (ev->key) {
    case TK_EOF:
        return KEY_EOF;

    case TK_RESIZE:
        return KEY_OK;

    case TK_TEXT:
        if (ev->text) {
            repl_insert_text(&p->repl, ev->text);
            free(ev->text);
            ev->text = NULL;
        }
        return KEY_OK;

    case TK_CHAR:
        if (ev->cp == 4) { /* Ctrl-D */
            if (p->repl.len == 0)
                return KEY_EOF;
            delete_forward(p);
            return KEY_OK;
        }
        if (ev->cp == 3 && live) /* Ctrl-C */
            return KEY_CANCEL;
        if (ev->cp == 22) { /* Ctrl-V */
            paste_clipboard(p, live);
            return KEY_OK;
        }
        if (ev->cp == 7) { /* Ctrl-G */
            edit_in_editor(p, live);
            return KEY_OK;
        }
        if (ev->cp == 12) { /* Ctrl-L */
            if (live)
                return KEY_OK;
            status_sticky_erased();
            p->painted_rows = 0;
            p->caret_row = 0;
            ui_esc("\x1b[2J\x1b[H");
            fflush(stdout);
            return KEY_OK;
        }
        feed(p, REPL_KEY_CHAR, ev->cp, NULL);
        return KEY_OK;

    case TK_TAB:
        /* Tab takes the highlighted completion wherever the cursor sits. With
         * the dropdown closed it first tries to open one — the cursor may have
         * moved back into a token — and failing that accepts the inline
         * suggestion, which repl.h binds to Right. */
        if (repl_accept_completion(&p->repl) || repl_open_completion(&p->repl))
            return KEY_OK;
        if (repl_suggestion(&p->repl))
            feed(p, REPL_KEY_RIGHT, 0, NULL);
        return KEY_OK;

    case TK_DELETE:
        delete_forward(p);
        return KEY_OK;

    case TK_ESCAPE:
        /* Escape still closes the dropdown; only an idle editor gives it up to
         * the turn. */
        if (live && !p->repl.dropdown_open)
            return KEY_CANCEL;
        feed(p, REPL_KEY_ESCAPE, 0, NULL);
        return KEY_OK;

    case TK_ENTER: {
        if (feed(p, REPL_KEY_ENTER, 0, NULL) != REPL_SUBMIT)
            return KEY_OK;
        const char *line = repl_line(&p->repl);
        return line && *line ? KEY_SUBMIT : KEY_OK;
    }

    default: {
        static const ReplKey MAP[] = {
            [TK_NEWLINE] = REPL_KEY_NEWLINE,   [TK_BACKSPACE] = REPL_KEY_BACKSPACE,
            [TK_LEFT] = REPL_KEY_LEFT,         [TK_RIGHT] = REPL_KEY_RIGHT,
            [TK_UP] = REPL_KEY_UP,             [TK_DOWN] = REPL_KEY_DOWN,
            [TK_WORD_LEFT] = REPL_KEY_WORD_LEFT,
            [TK_WORD_RIGHT] = REPL_KEY_WORD_RIGHT,
        };
        if (ev->key == TK_HOME)
            feed(p, REPL_KEY_CHAR, 1, NULL); /* ctrl-a */
        else if (ev->key == TK_END)
            feed(p, REPL_KEY_CHAR, 5, NULL); /* ctrl-e */
        else if ((size_t)ev->key < sizeof MAP / sizeof *MAP)
            feed(p, MAP[ev->key], 0, NULL);
        return KEY_OK;
    }
    }
}

/* Lift the submitted line out of the editor, recording it in history. The
 * editor is left empty and ready for the next one. */
static char *take_line(struct prompt *p)
{
    const char *line = repl_line(&p->repl);
    char *out = line && *line ? strdup(line) : NULL;
    if (out) {
        repl_history_add(&p->repl, out);
        history_append(p, out);
    }
    repl_reset(&p->repl);
    return out;
}

void prompt_set_idle(struct prompt *p, int (*fd)(void *ud),
                     void (*render)(void *ud), void *ud)
{
    p->idle_fd = fd;
    p->idle_render = render;
    p->idle_ud = ud;
}

/* The watch hooks tty_read installs for the wait inside prompt_read. The prompt
 * being read is the one on screen, so it can be reached from a static. */
static struct prompt *waiting;

static int idle_fd_hook(void *ud)
{
    (void)ud;
    return waiting && waiting->idle_fd ? waiting->idle_fd(waiting->idle_ud) : -1;
}

/* Print what arrived where the block is, not on top of it. */
static void idle_ready_hook(void *ud)
{
    (void)ud;
    struct prompt *p = waiting;
    if (!p || !p->idle_render)
        return;
    erase_block(p);
    p->idle_render(p->idle_ud);
    repaint(p);
}

static char *read_loop(struct prompt *p)
{
    /* Whatever was typed during the last turn but never submitted carries over,
     * so the block is repainted rather than reset. */
    p->painted_rows = 0;
    p->caret_row = 0;
    repaint(p);

    int resizing = 0;
    for (;;) {
        tty_event ev;
        /* Once the size stops changing, one repaint puts the block back. */
        if (!tty_read(&ev, resizing ? TTY_RESIZE_SETTLE_MS : -1)) {
            if (resizing) {
                resizing = 0;
                repaint(p);
            }
            continue;
        }
        if (ev.key == TK_RESIZE) {
            resizing = 1;
            continue;
        }
        resizing = 0;

        switch (feed_key(p, &ev, 0)) {
        case KEY_EOF:
            erase_block(p);
            return NULL;

        case KEY_SUBMIT: {
            char *out = take_line(p);
            erase_block(p);
            if (out)
                prompt_echo_message(out);
            return out;
        }

        default:
            repaint(p);
            continue;
        }
    }
}

char *prompt_read(struct prompt *p)
{
    /* Unprompted output is only welcome while this wait owns the screen: a turn
     * reads its own stream, and the pump would take it out from under it. */
    waiting = p;
    tty_watch(p->idle_fd ? idle_fd_hook : NULL, idle_ready_hook, NULL);
    char *out = read_loop(p);
    tty_watch(NULL, NULL, NULL);
    waiting = NULL;
    return out;
}

/* ---------- the prompt while a turn runs ---------- */

static void queue_push(struct prompt *p, char *line)
{
    if (!line)
        return;
    if (p->queued_count == p->queued_cap) {
        int cap = p->queued_cap ? p->queued_cap * 2 : 4;
        char **grown = realloc(p->queued, (size_t)cap * sizeof *grown);
        if (!grown) {
            free(line);
            return;
        }
        p->queued = grown;
        p->queued_cap = cap;
    }
    p->queued[p->queued_count++] = line;
}

char *prompt_take_queued(struct prompt *p)
{
    if (p->queued_count == 0)
        return NULL;
    char *line = p->queued[0];
    memmove(p->queued, p->queued + 1, (size_t)(--p->queued_count) * sizeof *p->queued);
    return line;
}

/* Up on an empty line pulls the newest queued message back into the editor
 * instead of browsing history: while something is waiting to run, that pending
 * message is what the user means to change. Submitting re-queues it. */
static int recall_queued(struct prompt *p)
{
    if (p->queued_count == 0)
        return 0;
    const char *live = repl_line(&p->repl);
    if (live && *live)
        return 0;

    char *line = p->queued[--p->queued_count];
    repl_reset(&p->repl);
    repl_insert_text(&p->repl, line);
    free(line);
    return 1;
}

void prompt_set_status(struct prompt *p, prompt_status_fn fn, void *ud)
{
    p->status = fn;
    p->status_ud = ud;
}

void prompt_set_live_command(struct prompt *p, prompt_live_fn fn, void *ud)
{
    p->live_command = fn;
    p->live_ud = ud;
}

int prompt_live_key(void *ud, tty_event *ev)
{
    struct prompt *p = ud;
    if (ev->key == TK_UP && recall_queued(p))
        return 0;
    switch (feed_key(p, ev, 1)) {
    case KEY_SUBMIT: {
        char *line = take_line(p);
        if (line && p->live_command && p->live_command(p->live_ud, line))
            free(line);
        else
            queue_push(p, line);
        return 0;
    }
    case KEY_EOF:
    case KEY_CANCEL:
        return 1;
    default:
        return 0;
    }
}

void prompt_live_paint(void *ud, int *rows, int *caret_row, int *caret_col)
{
    struct prompt *p = ud;
    p->live_block = 1;
    paint_block(p, rows, caret_row, caret_col);
    p->painted_rows = *rows;
}

int prompt_live_offset(void *ud)
{
    return caret_offset(ud, ui_columns());
}
