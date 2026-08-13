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
    UI_CHROME,  /* application frame */
    UI_DIM,     /* asides, footers, tool activity */
    UI_BODY,    /* the assistant's prose */
    UI_BOLD,    /* the accents within it: emphasis and code */
    UI_ITALIC,
    UI_CODE,    /* inline code and fenced blocks */
    UI_HEADING,
    UI_LINK,
    UI_ERROR,
    UI_OK,
    UI_TOOL,    /* the [name] tag on a tool call */
    UI_RESET,
};

#define UI_BAR "\xe2\x96\x8c" /* ▌ */

void        ui_init(void);
const char *ui_style(enum ui_role role);

/* The caret is the terminal's own cursor, so it only follows the theme if we
 * recolor it (OSC 12) and hand it back (OSC 112) before exiting. */
void ui_cursor_accent(void);
void ui_cursor_restore(void);

/* Newline-translating output. ui_raw(1) makes '\n' emit "\r\n". */
void ui_raw(int on);
void ui_put(const char *s);
void ui_putn(const char *s, size_t n);
__attribute__((format(printf, 1, 2))) void ui_printf(const char *fmt, ...);
void ui_flush(void);

/* Emit a control sequence verbatim, exempt from newline translation. */
void ui_esc(const char *s);

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
