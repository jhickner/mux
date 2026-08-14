/* Terminal output: styling, display-width measurement, wrapping, and the
 * gutter-bar chrome. Every write goes through here because the app stays in raw
 * mode, where a bare '\n' would step right instead of returning to column 0. */
#ifndef UI_H
#define UI_H

#include <stdarg.h>
#include <stddef.h>

/* SGR sequences, or "" when color is off. Use through ui_style() so NO_COLOR and
 * non-tty output stay clean. */
enum ui_role {
    UI_ACCENT,  /* the caret, and the user's words once submitted */
    UI_TEXT,    /* the user's words while being typed */
    UI_STICKY,  /* the floating copy of the prompt the turn is answering */
    UI_CHROME,  /* application frame */
    UI_DIM,     /* asides and footers */
    UI_BODY,    /* the assistant's prose */
    UI_BOLD,    /* the accents within it: emphasis and code */
    UI_ITALIC,
    UI_CODE,    /* inline code and fenced blocks */
    UI_HEADING,
    UI_LINK,
    UI_ERROR,
    UI_OK,
    UI_THINKING,/* the model's reasoning rows */
    UI_TOOL,    /* the [name] tag on a tool call */
    UI_RESET,
};

#define UI_BAR "\xe2\x96\x8c" /* ▌ */

void        ui_init(void);
const char *ui_style(enum ui_role role);

/* The typing block is the terminal's own cursor: hold it at the theme's plain
 * cursor color (OSC 12) and hand it back (OSC 112) before exiting. The accent
 * belongs to the ❯ caret, not to this. */
void ui_cursor_plain(void);
void ui_cursor_restore(void);

/* Newline-translating output. ui_raw(1) makes '\n' emit "\r\n". */
void ui_raw(int on);
void ui_put(const char *s);
void ui_putn(const char *s, size_t n);
__attribute__((format(printf, 1, 2))) void ui_printf(const char *fmt, ...);
void ui_flush(void);

/* Emit a control sequence verbatim, exempt from newline translation. */
void ui_esc(const char *s);

/* How far output has pushed the screen up since the mark, in rows — what a line
 * feed adds, plus the rows a long line wraps onto. Blocks that are repainted in
 * place scroll nothing in the end, so they bracket their drawing with
 * ui_scroll_track(0) and are left out of the count. */
void ui_scroll_mark(void);
int  ui_scroll_rows(void);
void ui_scroll_track(int on);

/* Synchronized output (DECSET 2026): brackets a repaint so the terminal presents
 * it as one frame. Without it an erase and the redraw that follows can be shown
 * separately, which is what a resize turns into visible tearing. */
void ui_sync_begin(void);
void ui_sync_end(void);

/* Physical rows that `count` logical rows of the given cell widths occupy at
 * `cols`. A row is re-wrapped by the terminal when the window narrows, so this
 * must be recomputed at the current width rather than remembered from the paint.
 * An empty row still occupies one row. */
int ui_reflow_rows(const int *row_widths, int count, int cols);

int    ui_columns(void);
size_t ui_cells(const char *s);              /* display width of UTF-8 text */
size_t ui_cells_n(const char *s, size_t n);

/* Bytes of `s` that fit within `budget` cells, preferring a break at the last
 * space. *skip receives the bytes to drop between rows (a consumed space or
 * newline), so the caller advances by row + skip. */
size_t ui_wrap_row(const char *s, size_t budget, size_t *skip);

/* Print `text` wrapped to the terminal width, every row indented by `indent`
 * cells and wrapped in `style` (which may be ""). Embedded newlines break rows.
 * The first row is indented too. */
void ui_wrapped(const char *text, int indent, const char *style);

/* One "▌ " chrome row: the bar in UI_CHROME, then the formatted text. */
__attribute__((format(printf, 2, 3))) void ui_bar(const char *style, const char *fmt, ...);

__attribute__((format(printf, 1, 2))) void ui_note(const char *fmt, ...);
__attribute__((format(printf, 1, 2))) void ui_error(const char *fmt, ...);

#endif /* UI_H */
