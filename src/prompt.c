#include "prompt.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app.h"
#include "bash.h"
#include "files.h"
#include "paste.h"
#include "settings.h"
#include "status.h"
#include "tty.h"
#include "ui.h"
#include "text.h"

#define REPL_STYLE_NONE ((signed char)-1)

#define QUEUE_ROWS_MAX 64

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
    int          painted_rows;
    int          painted_cols;
    int          caret_row;
    int          caret_col;
    int          caret_frame_row;
    char        *history_path;
    prompt_live_fn live_command;
    void          *live_ud;
    char       **queued;
    int          queued_count;
    int          queued_cap;
    char        *file_root;
    int        (*idle_fds)(void *ud, int *out, int max);
    int        (*idle_render)(void *ud);
    int        (*idle_busy)(void *ud);
    void        *idle_ud;
    void       (*replay)(void *ud);
    void        *replay_ud;
    int        (*restart_pending)(void *ud);
    int        (*restart)(void *ud);
    void        *restart_ud;
    int          live_block;
    int          frame_ok;
    char        *painted_head;
    int          painted_queued;
    int          above_widths[QUEUE_ROWS_MAX];
    int          above_rows;
};

static void history_append(struct prompt *p, const char *line)
{
    if (!p->history_path || !*line)
        return;
    FILE *f = fopen(p->history_path, "a");
    if (!f)
        return;

    for (const char *q = line; *q; q++)
        fputc(*q == '\n' ? ' ' : *q, f);
    fputc('\n', f);
    fclose(f);
}

void prompt_history_open(struct prompt *p, const char *path)
{
    free(p->history_path);
    p->history_path = strdup(path);
    if (!p->history_path)
        return;

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

static void frame_size(struct frame *f, int rows, int cols)
{
    if (rows < 0 || cols < 0 || (cols > 0 && rows > INT_MAX / cols)) {
        f->rows = f->cols = 0;
        return;
    }
    int need = rows * cols;
    if (need > f->cap) {
        struct cell *grown = realloc(f->cells, (size_t)need * sizeof *grown);
        if (!grown) {

            f->rows = f->cols = 0;
            return;
        }
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

static int rows_for(size_t cells, int cols)
{
    return cells ? (int)((cells - 1) / (size_t)cols) + 1 : 1;
}

static const char *head_text(const struct prompt *p)
{
    return p->live_block ? NULL : status_sticky_offscreen();
}

#define STICKY_DONE "\xe2\x9c\x93 "

#define STICKY_BUSY "\xe2\x8b\xaf "

static size_t sticky_budget(int cols)
{
    return (size_t)(cols - 5 > 4 ? cols - 5 : 4);
}

static struct ui_wrap bar_wrap(size_t budget, enum ui_role role, int cap,
                               const char *mark)
{
    struct ui_wrap w = {0};
    w.budget = budget;
    w.gutter = UI_BAR " ";
    w.mark = mark;
    w.role = role;
    w.max_rows = cap;
    w.erase = 1;
    w.paint_empty = 1;
    return w;
}

static int reflowed_rows(const char *text, size_t budget, int cols, int cap,
                         const char *mark)
{
    struct ui_wrap w = bar_wrap(budget, UI_RESET, cap, mark);
    w.measure = 1;
    w.reflow_cols = cols;
    return ui_wrap_paint(text, &w);
}

static int input_offset(const struct prompt *p, int cols)
{
    int up = 0;
    for (int y = 0; y < p->caret_frame_row && y < p->frame.rows; y++)
        up += rows_for((size_t)row_extent(&p->frame, y), cols);
    return up + p->caret_col / cols;
}

static int caret_offset(const struct prompt *p, int cols)
{
    if (p->painted_rows == 0 || cols == p->painted_cols || p->painted_cols <= 0)
        return p->caret_row;

    int up = 0;
    size_t budget = queued_budget(p->painted_cols);
    if (p->painted_head)
        up += reflowed_rows(p->painted_head, sticky_budget(p->painted_cols),
                            cols, STICKY_LINES, STICKY_DONE) + 1;
    for (int i = 0; i < p->painted_queued && i < p->queued_count; i++)
        up += reflowed_rows(p->queued[i], budget, cols, 0, NULL);

    return up + input_offset(p, cols);
}

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
    ui_esc(UI_ERASE_BELOW);
    fflush(stdout);
    p->painted_rows = 0;
    free(p->painted_head);
    p->painted_head = NULL;
    p->painted_queued = 0;
}

static void erase_input(struct prompt *p)
{
    if (p->painted_rows == 0)
        return;
    int up = input_offset(p, ui_columns());
    if (up > 0)
        ui_move(up, 'A');
    ui_esc("\r");
    ui_esc(UI_ERASE_BELOW);
    fflush(stdout);
    p->painted_rows = 0;
    free(p->painted_head);
    p->painted_head = NULL;
    p->painted_queued = 0;
}

static int caret_is_synthetic(const Repl *r)
{
    return r->cursor >= r->len || r->buf[r->cursor] == '\n';
}

// Narrow terminals floor the wrap budget above what fits, so a painted line can
// span several screen rows; measure with reflow so the offsets a repaint rewinds
// by match what the terminal actually holds.
static int rows_above(const struct prompt *p, const char *head, int cols, int show_queued)
{
    int rows = head ? reflowed_rows(head, sticky_budget(cols), cols, STICKY_LINES,
                                    STICKY_DONE) + 1 : 0;
    if (!show_queued)
        return rows;
    size_t budget = queued_budget(cols);
    for (int i = 0; i < p->queued_count; i++)
        rows += reflowed_rows(p->queued[i], budget, cols, 0, NULL);
    return rows;
}

// A block taller than the screen cannot be rewound: its top has already scrolled
// off, so each repaint stacks another copy into the scrollback. Drop the queued
// lines, then the sticky head, until what we paint fits.
static int fit_above(const struct prompt *p, const char **head, int *show_queued,
                     int cols, int input_rows)
{
    int limit = tty_rows() - 1;

    *show_queued = !p->live_block;
    int rows = rows_above(p, *head, cols, *show_queued);
    if (rows + input_rows <= limit)
        return rows;

    if (*show_queued) {
        *show_queued = 0;
        rows = rows_above(p, *head, cols, 0);
        if (rows + input_rows <= limit)
            return rows;
    }

    *head = NULL;
    return 0;
}

static void paint_bars(const char *text, size_t budget, enum ui_role role, int cap,
                       const char *mark)
{
    struct ui_wrap w = bar_wrap(budget, role, cap, mark);
    ui_wrap_paint(text, &w);
}

static void paint_above(const struct prompt *p, const char *head, int cols,
                        int show_queued)
{
    if (head) {
        int busy = p->idle_busy && p->idle_busy(p->idle_ud);
        paint_bars(head, sticky_budget(cols), busy ? UI_STICKY : UI_STICKY_DONE,
                   STICKY_LINES, busy ? STICKY_BUSY : STICKY_DONE);
        ui_esc(UI_ERASE_EOL);
        ui_put("\n");
    }
    if (!show_queued)
        return;
    size_t budget = queued_budget(cols);
    for (int i = 0; i < p->queued_count; i++)
        paint_bars(p->queued[i], budget, UI_DIM, 0, NULL);
}

static void emit_input(struct prompt *p, int rows)
{
    int synthetic = caret_is_synthetic(&p->repl);

    for (int y = 0; y < rows; y++) {
        ui_esc(UI_ERASE_EOL);
        int extent = row_extent(&p->frame, y);
        const char *open = "";
        for (int x = 0; x < extent; x++) {
            struct cell *c = &p->frame.cells[y * p->frame.cols + x];
            uint32_t cp = c->cp;
            const char *seq = c->style == REPL_STYLE_NONE
                                  ? "" : style_open((ReplStyle)c->style);

            if (c->style == REPL_STYLE_PROMPT && x == 0 && cp == '>') {
                cp = 0x276F;
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

    ui_esc(UI_ERASE_BELOW);
}

static void paint_block(struct prompt *p, int *rows_out, int *caret_row, int *caret_col)
{
    int cols = ui_columns();
    const char *head = head_text(p);

    if (p->frame_ok && cols == p->painted_cols && p->frame.cells && p->frame.rows > 0) {
        int input_rows = p->frame.rows;
        int show_queued = 0;
        int above = fit_above(p, &head, &show_queued, cols, input_rows);
        *rows_out = input_rows + above;
        *caret_row = above + (p->frame.have_cursor ? p->frame.cursor_y : input_rows - 1);
        *caret_col = p->frame.have_cursor ? p->frame.cursor_x : 0;
        paint_above(p, head, cols, show_queued);
        emit_input(p, input_rows);
        // The head can appear or change without the frame being rebuilt, and a
        // stale record of it drops its rows from the offset a resize repaints
        // against, stranding the old block on screen.
        free(p->painted_head);
        p->painted_head = head ? strdup(head) : NULL;
        p->painted_queued = show_queued ? p->queued_count : 0;
        p->caret_row = *caret_row;
        p->caret_col = *caret_col;
        p->caret_frame_row = p->frame.have_cursor ? p->frame.cursor_y : input_rows - 1;
        return;
    }

    int input_rows = repl_input_rows(&p->repl, cols);
    int rows = input_rows + repl_dropdown_rows(&p->repl);
    if (rows < 1)
        rows = 1;

    int show_queued = 0;
    int above = fit_above(p, &head, &show_queued, cols, rows);
    *rows_out = rows + above;
    *caret_row = above;
    *caret_col = 0;

    frame_size(&p->frame, rows, cols);
    if (!p->frame.cells || p->frame.rows < rows || p->frame.cols < cols) {
        *rows_out = 1;
        *caret_row = 0;
        p->painted_cols = cols;
        p->frame_ok = 0;
        free(p->painted_head);
        p->painted_head = NULL;
        p->painted_queued = 0;
        p->caret_row = p->caret_col = p->caret_frame_row = 0;
        return;
    }
    repl_render(&p->repl, 0, 0, cols, true, draw_cell, &p->frame);

    paint_above(p, head, cols, show_queued);

    *caret_row = above;
    *caret_row += p->frame.have_cursor ? p->frame.cursor_y : rows - 1;
    *caret_col = p->frame.have_cursor ? p->frame.cursor_x : 0;

    p->painted_cols = cols;
    p->frame_ok = 1;
    free(p->painted_head);
    p->painted_head = head ? strdup(head) : NULL;
    p->painted_queued = show_queued ? p->queued_count : 0;
    p->caret_row = *caret_row;
    p->caret_col = *caret_col;
    p->caret_frame_row = p->frame.have_cursor ? p->frame.cursor_y : rows - 1;

    emit_input(p, rows);
}

static void repaint(struct prompt *p)
{
    int rows = 1, caret_row = 0, caret_col = 0;

    p->live_block = 0;

    // Nothing we paint would fit, and rewinding geometry the terminal has
    // already reflowed only strands copies of the block. Sit the resize out and
    // start clean once there is room again.
    if (ui_too_narrow()) {
        p->painted_rows = 0;
        p->caret_row = p->caret_col = 0;
        p->frame_ok = 0;
        return;
    }

    ui_sync_begin();

    ui_scroll_track(0);
    goto_origin(p);
    ui_esc(UI_CURSOR_HIDE);
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
    ui_esc(UI_CURSOR_SHOW);
    ui_scroll_track(1);
    ui_sync_end();
    fflush(stdout);

    p->painted_rows = rows;
}

void prompt_echo_message(const char *text)
{
    int cols = ui_columns();
    enum ui_role role = bash_is_command(text) ? UI_BASH : UI_ECHO;

    int cap = settings_get_int(SETTING_ECHO_ROWS, ECHO_ROWS_DEFAULT);
    struct ui_wrap w = bar_wrap(queued_budget(cols), role, cap > 0 ? cap : 0, NULL);
    ui_wrap_paint(text, &w);
    ui_put("\n");
    ui_flush();
}

struct prompt *prompt_new(const ReplCommand *commands, int command_count)
{
    struct prompt *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    repl_init(&p->repl, commands, command_count);

    p->repl.suggest_off = true;
    return p;
}

static struct prompt *completion_owner;

void prompt_file_completion(struct prompt *p, const char *root)
{
    completion_owner = p;
    free(p->file_root);
    p->file_root = strdup(root);
    if (p->file_root)
        repl_set_completer(&p->repl, files_complete, p->file_root);
    files_prefetch(p->file_root);
}

void prompt_rehome(const char *root)
{
    if (completion_owner && root && *root)
        prompt_file_completion(completion_owner, root);
}

void prompt_free(struct prompt *p)
{
    if (!p)
        return;
    if (completion_owner == p)
        completion_owner = NULL;
    repl_free(&p->repl);
    files_forget();
    free(p->frame.cells);
    free(p->file_root);
    free(p->history_path);
    free(p->painted_head);
    for (int i = 0; i < p->queued_count; i++)
        free(p->queued[i]);
    free(p->queued);
    free(p);
}

static int feed(struct prompt *p, ReplKey key, uint32_t cp, const char *text)
{
    ReplEvent ev = {.key = key, .codepoint = cp, .text = text};
    repl_set_width(&p->repl, ui_columns());
    p->frame_ok = 0;
    return repl_handle_input(&p->repl, &ev);
}

static void delete_forward(struct prompt *p)
{
    if (p->repl.dropdown_open || p->repl.cursor >= p->repl.len)
        return;
    feed(p, REPL_KEY_RIGHT, 0, NULL);
    feed(p, REPL_KEY_BACKSPACE, 0, NULL);
}

static void paste_clipboard(struct prompt *p, int live)
{
    char path[1024];
    if (paste_image(path, sizeof path - 1)) {
        size_t n = strlen(path);
        path[n] = ' ';
        path[n + 1] = '\0';
        p->frame_ok = 0;
        repl_insert_text(&p->repl, path);
        return;
    }

    char *text = paste_text();
    if (text) {
        p->frame_ok = 0;
        repl_insert_text(&p->repl, text);
        free(text);
        return;
    }

    if (live)
        return;
    erase_block(p);
    ui_note("clipboard is empty");
    repaint(p);
}

static int editor_temp(char *out, size_t size)
{
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir)
        dir = "/tmp";
    if (strchr(dir, '\''))
        return 0;
    if ((size_t)snprintf(out, size, "%s/" APP_NAME "-XXXXXX", dir) >= size)
        return 0;
    int fd = mkstemp(out);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

static char *read_whole(const char *path)
{
    size_t got = 0;
    char *buf = text_slurp(path, 0, &got);
    if (!buf)
        return NULL;

    if (got && buf[got - 1] == '\n')
        buf[--got] = '\0';
    if (got && buf[got - 1] == '\r')
        buf[--got] = '\0';
    return buf;
}

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

    if (tty_raw_begin() != 0) {

        fprintf(stderr, "could not return the terminal to raw mode\n");
        exit(1);
    }
    ui_raw(1);
    ui_cursor_plain();

    int ok = status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (ok) {
        char *text = read_whole(path);
        if (text) {
            p->frame_ok = 0;
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
    KEY_OK,
    KEY_SUBMIT,
    KEY_EOF,
    KEY_CANCEL,
};

static void cycle_colors(struct prompt *p, int live, int mine)
{
    const char *label = ui_cycle(mine ? UI_GROUP_INPUT : UI_GROUP_EMPHASIS, 1);

    if (live)
        status_pause();
    else
        erase_block(p);

    p->frame_ok = 0;
    if (p->replay) {
        status_sticky_erased();
        p->painted_rows = 0;
        p->caret_row = 0;
        p->replay(p->replay_ud);
    }
    ui_esc(ui_style(UI_BRAND));
    ui_put(UI_BAR);
    ui_esc(ui_style(mine ? UI_ACCENT : UI_BOLD));
    ui_printf(" %s: %s", mine ? "your input" : "reply highlights", label);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
    ui_flush();

    if (live) {
        status_resume();
        status_touch();
    } else {
        repaint(p);
    }
}

static enum key_result feed_key(struct prompt *p, tty_event *ev, int live)
{
    switch (ev->key) {
    case TK_EOF:
        return KEY_EOF;

    case TK_RESIZE:
        return KEY_OK;

    case TK_TEXT:
        if (ev->text) {
            p->frame_ok = 0;
            repl_insert_text(&p->repl, ev->text);
            free(ev->text);
            ev->text = NULL;
        }
        return KEY_OK;

    case TK_CHAR:
        if (ev->cp == 4) {
            if (p->repl.len == 0)
                return KEY_EOF;
            delete_forward(p);
            return KEY_OK;
        }
        if (ev->cp == 3 && live && p->repl.len == 0 &&
            !p->repl.dropdown_open && !p->repl.searching)
            return KEY_CANCEL;
        if (ev->cp == 22) {
            paste_clipboard(p, live);
            return KEY_OK;
        }
        if (ev->cp == 7) {
            edit_in_editor(p, live);
            return KEY_OK;
        }
        if (ev->cp == 14 || ev->cp == 15) {
            cycle_colors(p, live, ev->cp == 14);
            return KEY_OK;
        }
        if (ev->cp == 12) {
            if (live)
                return KEY_OK;
            status_sticky_erased();
            p->painted_rows = 0;
            p->caret_row = 0;
            ui_esc(UI_CLEAR_SCREEN);
            fflush(stdout);
            return KEY_OK;
        }
        feed(p, REPL_KEY_CHAR, ev->cp, NULL);
        return KEY_OK;

    case TK_TAB:

        if (repl_accept_completion(&p->repl) || repl_open_completion(&p->repl))
            return KEY_OK;
        if (repl_suggestion(&p->repl))
            feed(p, REPL_KEY_RIGHT, 0, NULL);
        return KEY_OK;

    case TK_DELETE:
        delete_forward(p);
        return KEY_OK;

    case TK_ESCAPE:

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
            feed(p, REPL_KEY_CHAR, 1, NULL);
        else if (ev->key == TK_END)
            feed(p, REPL_KEY_CHAR, 5, NULL);
        else if ((size_t)ev->key < sizeof MAP / sizeof *MAP)
            feed(p, MAP[ev->key], 0, NULL);
        return KEY_OK;
    }
    }
}

static char *take_line(struct prompt *p)
{
    const char *line = repl_line(&p->repl);
    char *out = line && *line ? strdup(line) : NULL;
    p->frame_ok = 0;
    if (out) {
        repl_history_add(&p->repl, out);
        history_append(p, out);
    }
    repl_reset(&p->repl);
    return out;
}

void prompt_set_idle(struct prompt *p, int (*fds)(void *ud, int *out, int max),
                     int (*render)(void *ud), int (*busy)(void *ud), void *ud)
{
    p->idle_fds = fds;
    p->idle_render = render;
    p->idle_busy = busy;
    p->idle_ud = ud;
}

void prompt_set_restart(struct prompt *p, int (*pending)(void *ud), int (*run)(void *ud),
                        void *ud)
{
    p->restart_pending = pending;
    p->restart = run;
    p->restart_ud = ud;
}

static int idle_fds_hook(void *ud, int *out, int max)
{
    struct prompt *p = ud;
    return p && p->idle_fds ? p->idle_fds(p->idle_ud, out, max) : 0;
}

static void idle_ready_hook(void *ud)
{
    struct prompt *p = ud;
    if (!p || !p->idle_render)
        return;
    erase_block(p);
    p->idle_render(p->idle_ud);
    repaint(p);
}

// A half-typed line or a queued message is work the restart would throw away,
// so it waits: the request is sticky and the next idle moment picks it up.
static void restart_check(struct prompt *p)
{
    if (!p->restart || p->repl.len || p->queued_count)
        return;
    if (!p->restart_pending(p->restart_ud))
        return;
    erase_block(p);
    ui_flush();
    p->restart(p->restart_ud);
    p->painted_rows = 0;
    p->caret_row = 0;
    repaint(p);
}

static char *read_loop(struct prompt *p)
{

    p->painted_rows = 0;
    p->caret_row = 0;
    repaint(p);
    // Covers a request that landed mid-turn, when nothing was waiting in select.
    restart_check(p);

    int resizing = 0;
    for (;;) {
        tty_event ev;

        if (!tty_read(&ev, resizing ? TTY_RESIZE_SETTLE_MS : -1)) {
            if (resizing) {
                resizing = 0;
                repaint(p);
            } else {
                restart_check(p);
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
            erase_input(p);
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
            // Picks up a request that was held back by a line the user has
            // now cleared, without waiting for the next wakeup.
            restart_check(p);
            continue;
        }
    }
}

char *prompt_read(struct prompt *p)
{

    tty_watch(p->idle_fds ? idle_fds_hook : NULL, idle_ready_hook, p);
    char *out = read_loop(p);
    tty_watch(NULL, NULL, NULL);
    return out;
}

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
    p->frame_ok = 0;
}

char *prompt_take_queued(struct prompt *p)
{
    if (p->queued_count == 0)
        return NULL;
    char *line = p->queued[0];
    memmove(p->queued, p->queued + 1, (size_t)(--p->queued_count) * sizeof *p->queued);
    return line;
}

static int recall_queued(struct prompt *p)
{
    if (p->queued_count == 0)
        return 0;
    const char *live = repl_line(&p->repl);
    if (live && *live)
        return 0;

    char *line = p->queued[--p->queued_count];
    p->frame_ok = 0;
    repl_reset(&p->repl);
    repl_insert_text(&p->repl, line);
    free(line);
    return 1;
}

void prompt_set_replay(struct prompt *p, void (*fn)(void *ud), void *ud)
{
    p->replay = fn;
    p->replay_ud = ud;
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

// Queued lines sit above the spinner, separated from it by a blank row.
void prompt_queue_paint(void *ud)
{
    struct prompt *p = ud;

    p->above_rows = 0;
    if (p->queued_count == 0)
        return;

    int cols = ui_columns();
    size_t budget = queued_budget(cols);
    int room = status_rows_left();
    int used = 0;
    for (int i = 0; i < p->queued_count && p->above_rows < QUEUE_ROWS_MAX; i++) {
        int need = reflowed_rows(p->queued[i], budget, cols, 0, NULL);
        if (used + need > room)
            break;
        used += need;
        struct ui_wrap w = bar_wrap(budget, UI_DIM, 0, NULL);
        w.widths = p->above_widths + p->above_rows;
        w.widths_max = QUEUE_ROWS_MAX - p->above_rows;
        int rows = ui_wrap_paint(p->queued[i], &w);
        p->above_rows += rows;
        if (p->above_rows > QUEUE_ROWS_MAX)
            p->above_rows = QUEUE_ROWS_MAX;
    }

    ui_esc(UI_ERASE_EOL);
    ui_put("\n");
    if (p->above_rows < QUEUE_ROWS_MAX)
        p->above_widths[p->above_rows++] = 0;
}

int prompt_queue_rows(void *ud)
{
    struct prompt *p = ud;
    return p->above_rows ? ui_reflow_rows(p->above_widths, p->above_rows, ui_columns()) : 0;
}
