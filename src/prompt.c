#include "prompt.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app.h"
#include "bash.h"
#include "chrome.h"
#include "block.h"
#include "files.h"
#include "paste.h"
#include "scrollback.h"
#include "settings.h"
#include "sidechannel.h"
#include "status.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"
#include "vendor/cJSON.h"
#include "text.h"

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
    int          painted_cols;
    int          above_painted;
    char        *history_path;
    prompt_live_fn live_command;
    void          *live_ud;
    int          (*echo_filter)(void *ud, const char *line);
    void          *echo_ud;
    char       **queued;
    int          queued_count;
    int          queued_cap;
    char        *file_root;
    char      *(*external)(void *ud);
    void        *external_ud;
    int          external_taken;
    int        (*idle_fds)(void *ud, int *out, int max);
    int        (*idle_render)(void *ud);
    int        (*idle_busy)(void *ud);
    void        *idle_ud;
    void       (*replay)(void *ud);
    void        *replay_ud;
    void       (*blank)(void *ud);
    void        *blank_ud;
    int        (*animate_busy)(void *ud);
    void       (*animate_tick)(void *ud);
    void        *animate_ud;
    int        (*restart_pending)(void *ud);
    int        (*restart)(void *ud);
    void        *restart_ud;
    int        (*takeover_pending)(void *ud);
    void       (*takeover)(void *ud);
    void        *takeover_ud;
    void       (*switcher)(void *ud);
    void        *switcher_ud;
    void       (*split)(void *ud, int quiet);
    void        *split_ud;
    void       (*another)(void *ud);
    void        *another_ud;
    int        (*cancel)(void *ud);
    void        *cancel_ud;
    int          stopped;
    int          frame_ok;
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
    ui_putn(buf, text_utf8_encode(cp, buf));
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

static struct ui_wrap bar_wrap(size_t budget, enum ui_role role, int cap,
                               const char *mark)
{
    struct ui_wrap w = {0};
    // The ellipsis is written past the budget, so a capped block keeps a cell
    // back for it or it lands off the row and is clipped away.
    if (cap > 0 && budget > 1)
        budget--;
    w.budget = budget;
    w.gutter = UI_BAR " ";
    w.mark = mark;
    w.role = role;
    w.max_rows = cap;
    w.erase = 1;
    w.paint_empty = 1;
    return w;
}

static int painted_rows(const char *text, size_t budget, int cap, const char *mark)
{
    struct ui_wrap w = bar_wrap(budget, UI_RESET, cap, mark);
    w.measure = 1;
    return ui_wrap_paint(text, &w);
}

static int caret_is_synthetic(const Repl *r)
{
    return r->cursor >= r->len || r->buf[r->cursor] == '\n';
}

static void paint_bars(const char *text, size_t budget, enum ui_role role, int cap,
                       const char *mark)
{
    struct ui_wrap w = bar_wrap(budget, role, cap, mark);
    ui_wrap_paint(text, &w);
}

int prompt_busy(struct prompt *p)
{
    return p && p->idle_busy && p->idle_busy(p->idle_ud);
}

int prompt_queued_rows(struct prompt *p, int cols)
{
    if (!p || p->queued_count == 0)
        return 0;
    size_t budget = queued_budget(cols);
    int rows = p->queued_count - 1;      /* the blank between each pair */
    for (int i = 0; i < p->queued_count; i++)
        rows += painted_rows(p->queued[i], budget, QUEUED_LINES, NULL);
    return rows;
}

void prompt_paint_queued(struct prompt *p, int room)
{
    if (!p || p->queued_count == 0)
        return;
    size_t budget = queued_budget(ui_columns());
    int used = 0;
    for (int i = 0; i < p->queued_count; i++) {
        int need = painted_rows(p->queued[i], budget, QUEUED_LINES, NULL);
        if (i)
            need++;                      /* the blank above this one */
        if (used + need > room)
            break;
        used += need;
        if (i)
            ui_put("\n");
        paint_bars(p->queued[i], budget, UI_DIM, QUEUED_LINES, NULL);
    }
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
}

// Renders the input into the frame and reports its rows. The frame is kept.
int prompt_input_rows(struct prompt *p, int cols)
{
    if (!p)
        return 1;
    if (p->frame_ok && cols == p->painted_cols && p->frame.cells && p->frame.rows > 0)
        return p->frame.rows;

    int rows = repl_input_rows(&p->repl, cols) + repl_dropdown_rows(&p->repl);
    if (rows < 1)
        rows = 1;

    frame_size(&p->frame, rows, cols);
    p->painted_cols = cols;
    if (!p->frame.cells || p->frame.rows < rows || p->frame.cols < cols) {
        p->frame_ok = 0;
        return 1;
    }
    repl_render(&p->repl, 0, 0, cols, true, draw_cell, &p->frame);
    p->frame_ok = 1;
    return rows;
}

void prompt_paint_input(struct prompt *p, int rows, int *caret_row, int *caret_col)
{
    *caret_row = 0;
    *caret_col = 0;
    if (!p || !p->frame_ok) {
        ui_put("");
        return;
    }
    emit_input(p, rows);
    *caret_row = p->frame.have_cursor ? p->frame.cursor_y : rows - 1;
    *caret_col = p->frame.have_cursor ? p->frame.cursor_x : 0;
}

static void repaint(struct prompt *p)
{
    (void)p;
    chrome_paint();
}

struct echo_item {
    char        *text;
    enum ui_role role;
    int          cap;
    int          gap;           /* owed a blank row above */
};

static void echo_paint(const struct echo_item *e)
{
    // Settled when the entry was made, not per redraw.
    if (e->gap)
        ui_put("\n");

    struct ui_wrap w = bar_wrap(queued_budget(ui_columns()), e->role, e->cap, NULL);
    ui_wrap_paint(e->text, &w);
    // A blank row under it, except for a shell command: its output starts on
    // the next row.
    if (e->role != UI_BASH)
        ui_put("\n");
}

static char *echo_encode(void *ud)
{
    const struct echo_item *e = ud;
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "text", e->text ? e->text : "");
    cJSON_AddNumberToObject(o, "role", e->role);
    cJSON_AddNumberToObject(o, "cap", e->cap);
    cJSON_AddNumberToObject(o, "gap", e->gap);
    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return out;
}

static void echo_render(void *ud, int cols)
{
    (void)cols;
    echo_paint(ud);
}

static void echo_free(void *ud)
{
    struct echo_item *e = ud;
    free(e->text);
    free(e);
}

void prompt_echo_message(const char *text)
{
    int cap = settings_get_int(SETTING_ECHO_ROWS, ECHO_ROWS_DEFAULT);

    int gap = !viewport_ends_blank();

    struct echo_item *e = malloc(sizeof *e);
    if (e) {
        e->text = strdup(text ? text : "");
        e->role = bash_is_command(text) ? UI_BASH : UI_ECHO;
        e->cap = cap > 0 ? cap : 0;
        e->gap = gap;
        if (!e->text) {
            free(e);
            e = NULL;
        }
    }
    // Kept, so the bar and its wrap are laid out again at a new width.
    if (e) {
        unsigned mark = viewport_item_begin(echo_render, e, echo_free);
        echo_paint(e);
        viewport_item_end();
        viewport_item_persist(mark, PROMPT_ECHO_KIND, echo_encode);
    } else {
        struct echo_item fallback = {(char *)(text ? text : ""),
                                     bash_is_command(text) ? UI_BASH : UI_ECHO,
                                     cap > 0 ? cap : 0, gap};
        echo_paint(&fallback);
    }
    ui_flush();
}

void prompt_echo_load(const cJSON *st)
{
    struct echo_item *e = malloc(sizeof *e);
    if (!e)
        return;
    e->text = strdup(scrollback_str(st, "text"));
    e->role = (enum ui_role)scrollback_int(st, "role");
    e->cap = scrollback_int(st, "cap");
    e->gap = scrollback_int(st, "gap");
    if (!e->text) {
        free(e);
        return;
    }
    unsigned mark = viewport_item_begin(echo_render, e, echo_free);
    echo_paint(e);
    viewport_item_end();
    viewport_item_persist(mark, PROMPT_ECHO_KIND, echo_encode);
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
            ui_note("could not create a temp file for $EDITOR");
            repaint(p);
        }
        return;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        unlink(path);
        if (!live) {
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
            ui_note("could not write %s", path);
            repaint(p);
        }
        return;
    }

    // $EDITOR is user config and may carry its own arguments, so it goes in as
    // written; only the path is quoted.
    char quoted[sizeof path * 4 + 3];
    char cmd[4096];
    int  len = text_shell_quote(path, quoted, sizeof quoted)
                   ? snprintf(cmd, sizeof cmd, "%s %s", editor, quoted)
                   : -1;
    if (len < 0 || (size_t)len >= sizeof cmd) {
        unlink(path);
        if (!live) {
            ui_note("$EDITOR command is too long");
            repaint(p);
        }
        return;
    }

    if (live)
        status_pause();
    else
        chrome_clear();
    viewport_suspend();
    ui_raw(0);
    tty_raw_end();

    int status = system(cmd);

    if (tty_raw_begin() != 0) {

        fprintf(stderr, "could not return the terminal to raw mode\n");
        exit(1);
    }
    ui_raw(1);
    ui_cursor_plain();
    block_forget();
    viewport_resume();

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

    p->frame_ok = 0;
    if (p->replay) {
        status_sticky_erased();
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
        if (ev->cp == 3 && p->repl.len == 0 &&
            !p->repl.dropdown_open && !p->repl.searching) {
            if (live)
                return KEY_CANCEL;
            // Nothing typed and a turn running behind the prompt: the key
            // belongs to the turn.
            if (p->cancel && p->cancel(p->cancel_ud))
                return KEY_OK;
        }
        if (ev->cp == 22) {
            paste_clipboard(p, live);
            return KEY_OK;
        }
        if (ev->cp == 7) {
            edit_in_editor(p, live);
            return KEY_OK;
        }
        // A shell beside this one, where the session is working. Nothing is
        // said about it mid-turn: the stream owns the screen then.
        if (ev->cp == 20) {
            if (p->split)
                p->split(p->split_ud, live);
            if (!live)
                repaint(p);
            return KEY_OK;
        }
        // Another session beside this one. Mid-turn the stream owns the
        // screen, so the key is ignored then.
        if (ev->cp == 2) {
            if (!live && p->another) {
                chrome_clear();
                p->another(p->another_ud);
            }
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
            block_cleared();
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
        if (!p->repl.dropdown_open && !p->repl.searching && p->repl.len == 0 &&
            p->cancel && p->cancel(p->cancel_ud))
            return KEY_OK;
        feed(p, REPL_KEY_ESCAPE, 0, NULL);
        return KEY_OK;

    case TK_ENTER: {
        // The repl consumes enter on an empty line, so it never submits one.
        // It works mid-turn too, which is when the status is most worth asking
        // for; the spinner steps aside the way a live command's echo does.
        if (p->blank && p->repl.len == 0 && !p->repl.dropdown_open &&
            !p->repl.searching) {
            if (live)
                status_pause();
            chrome_clear();
            p->blank(p->blank_ud);
            if (live)
                status_resume();
            return KEY_OK;
        }
        if (feed(p, REPL_KEY_ENTER, 0, NULL) != REPL_SUBMIT)
            return KEY_OK;
        const char *line = repl_line(&p->repl);
        return line && *line ? KEY_SUBMIT : KEY_OK;
    }

    case TK_PAGE_UP:
        viewport_scroll(tty_rows() / 2);
        return KEY_OK;

    case TK_PAGE_DOWN:
        viewport_scroll(-(tty_rows() / 2));
        return KEY_OK;

    case TK_SCROLL_UP:
        viewport_scroll(3);
        return KEY_OK;

    case TK_SCROLL_DOWN:
        viewport_scroll(-3);
        return KEY_OK;

    default: {
        // Left with nothing typed is not a cursor move: it is the way out of
        // this conversation into the list of all of them.
        if (ev->key == TK_LEFT && !live && p->switcher && p->repl.len == 0 &&
            !p->repl.dropdown_open && !p->repl.searching) {
            chrome_clear();
            p->switcher(p->switcher_ud);
            return KEY_OK;
        }

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
    p->idle_render(p->idle_ud);
    repaint(p);
}

// A restart would throw away a half-typed line or a queued one, so the request
// is sticky and waits for a moment with neither.
static void restart_check(struct prompt *p)
{
    if (!p->restart || p->repl.len || p->queued_count)
        return;
    if (!p->restart_pending(p->restart_ud))
        return;
    chrome_clear();
    ui_flush();
    p->restart(p->restart_ud);
    repaint(p);
}

void prompt_set_takeover(struct prompt *p, int (*pending)(void *ud), void (*run)(void *ud),
                         void *ud)
{
    p->takeover_pending = pending;
    p->takeover = run;
    p->takeover_ud = ud;
}

void prompt_set_cancel(struct prompt *p, int (*fn)(void *ud), void *ud)
{
    p->cancel = fn;
    p->cancel_ud = ud;
}

void prompt_set_switcher(struct prompt *p, void (*fn)(void *ud), void *ud)
{
    p->switcher = fn;
    p->switcher_ud = ud;
}

void prompt_set_another(struct prompt *p, void (*fn)(void *ud), void *ud)
{
    p->another = fn;
    p->another_ud = ud;
}

void prompt_set_split(struct prompt *p, void (*fn)(void *ud, int quiet), void *ud)
{
    p->split = fn;
    p->split_ud = ud;
}

void prompt_stop(struct prompt *p)
{
    if (p)
        p->stopped = 1;
}

// Unlike a restart, this cannot wait for a quiet prompt: the window asking for
// the session is waiting on it.
static void takeover_check(struct prompt *p)
{
    if (!p->takeover || !p->takeover_pending || !p->takeover_pending(p->takeover_ud))
        return;
    chrome_clear();
    ui_flush();
    p->takeover(p->takeover_ud);
    repaint(p);
}

void prompt_restart_check(struct prompt *p)
{
    if (p)
        restart_check(p);
}

static void queue_push(struct prompt *p, char *line);

static char *read_loop(struct prompt *p)
{
    repaint(p);
    takeover_check(p);
    restart_check(p);

    int resizing = 0;
    for (;;) {
        tty_event ev;

        if (p->stopped) {
            p->stopped = 0;
            chrome_clear();
            return NULL;
        }

        // A line from somewhere other than the keyboard — the chat bridge. It
        // submits as typed when nothing is half-written, and waits its turn
        // behind the line being composed when something is.
        if (p->external) {
            char *line = p->external(p->external_ud);
            if (line) {
                const char *composing = repl_line(&p->repl);
                if (composing && *composing) {
                    queue_push(p, line);
                    repaint(p);
                    continue;
                }
                p->external_taken = 1;
                chrome_clear();
                if (prompt_echoes(p, line))
                    prompt_echo_message(line);
                return line;
            }
        }

        // Something animating needs waking on a frame, not on a keystroke.
        int animating = !resizing && p->animate_busy && p->animate_busy(p->animate_ud);
        int wait = resizing ? TTY_RESIZE_SETTLE_MS : (animating ? SPIN_FRAME_MS : -1);

        if (!tty_read(&ev, wait)) {
            if (resizing) {
                resizing = 0;
                // Goes over whatever else drew on the pane while it resized.
                viewport_forget();
                repaint(p);
            } else {
                if (animating && p->animate_tick) {
                    p->animate_tick(p->animate_ud);
                    repaint(p);
                }
                takeover_check(p);
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
            chrome_keep_above();
            return NULL;

        case KEY_SUBMIT: {
            p->external_taken = 0;
            char *out = take_line(p);
            chrome_clear();
            if (out && prompt_echoes(p, out))
                prompt_echo_message(out);
            return out;
        }

        default:
            repaint(p);
            takeover_check(p);
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

void prompt_set_external(struct prompt *p, char *(*fn)(void *ud), void *ud)
{
    p->external = fn;
    p->external_ud = ud;
}

int prompt_line_was_external(struct prompt *p)
{
    return p && p->external_taken;
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

void prompt_set_animate(struct prompt *p, int (*busy)(void *ud), void (*tick)(void *ud),
                        void *ud)
{
    p->animate_busy = busy;
    p->animate_tick = tick;
    p->animate_ud = ud;
}

void prompt_set_replay(struct prompt *p, void (*fn)(void *ud), void *ud)
{
    p->replay = fn;
    p->replay_ud = ud;
}

void prompt_set_blank(struct prompt *p, void (*fn)(void *ud), void *ud)
{
    p->blank = fn;
    p->blank_ud = ud;
}

void prompt_set_echo_filter(struct prompt *p, int (*fn)(void *ud, const char *line),
                            void *ud)
{
    p->echo_filter = fn;
    p->echo_ud = ud;
}

int prompt_echoes(struct prompt *p, const char *line)
{
    return !p || !p->echo_filter || p->echo_filter(p->echo_ud, line);
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

