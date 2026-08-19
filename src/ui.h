
#ifndef UI_H
#define UI_H

#include <stddef.h>

enum ui_role {
    UI_ACCENT,
    UI_ECHO,
    UI_TEXT,
    UI_STICKY,
    UI_STICKY_DONE,
    UI_BRAND,
    UI_SIDE,
    UI_CHROME,
    UI_DIM,
    UI_BODY,
    UI_BOLD,
    UI_ITALIC,
    UI_CODE,
    UI_HEADING,
    UI_LINK,
    UI_ERROR,
    UI_OK,
    UI_THINKING,
    UI_TOOL,
    UI_SPIN,
    UI_BASH,
    UI_SYN_CMD,
    UI_SYN_KEYWORD,
    UI_SYN_STRING,
    UI_SYN_COMMENT,
    UI_SYN_VAR,
    UI_SYN_OP,
    UI_SYN_FLAG,
    UI_SYN_NUMBER,
    UI_RESET,
};

enum ui_group {
    UI_GROUP_INPUT,
    UI_GROUP_EMPHASIS,
};

#define UI_BAR "\xe2\x96\x8c"

#define UI_CURSOR_SHOW  "\x1b[?25h"
#define UI_CURSOR_HIDE  "\x1b[?25l"
#define UI_ERASE_EOL    "\x1b[K"
#define UI_ERASE_BELOW  "\x1b[J"
#define UI_CLEAR_SCREEN "\x1b[2J\x1b[H"
#define UI_PASTE_ON     "\x1b[?2004h"
#define UI_PASTE_OFF    "\x1b[?2004l"

#define UI_ALT_ON       "\x1b[?1049h"
#define UI_ALT_OFF      "\x1b[?1049l"
#define UI_HOME         "\x1b[H"

void        ui_init(void);

void ui_pad(int cells);

const char *ui_style(enum ui_role role);

int ui_color(void);

const char *ui_cycle(enum ui_group group, int delta);

void ui_cursor_plain(void);
void ui_cursor_restore(void);

void ui_raw(int on);
void ui_put(const char *s);
void ui_putn(const char *s, size_t n);
__attribute__((format(printf, 1, 2))) void ui_printf(const char *fmt, ...);
void ui_flush(void);

void ui_esc(const char *s);

void ui_sync_begin(void);
void ui_sync_end(void);

int ui_reflow_rows(const int *row_widths, int count, int cols);

int    ui_columns(void);
int    ui_screen_columns(void);
int    ui_too_narrow(void);
size_t ui_cells(const char *s);
size_t ui_cells_n(const char *s, size_t n);

size_t ui_wrap_row(const char *s, size_t n, size_t budget, size_t *skip, size_t *cells);

size_t ui_fit_bytes(const char *s, size_t budget);

size_t ui_cells_visible(const char *s, size_t n);

// ui_cells_visible over a stream. Producers are free to split an escape or a
// codepoint between writes, so the state that says "still inside one" has to
// outlive the call; zero it to start a fresh stream.
struct ui_cellstream {
    unsigned char state;
    unsigned char pending[4];
    unsigned char pending_n;
};

size_t ui_cells_stream(struct ui_cellstream *st, const char *s, size_t n);

size_t ui_fit_visible(const char *s, size_t n, size_t budget);

void ui_put_spans(const char *s, size_t n, const unsigned char *roles, enum ui_role base);

void  ui_capture_begin(int columns);
char *ui_capture_end(void);

void  ui_sink_begin(void);
int   ui_sink_rows(void);
char *ui_sink_end(void);

struct ui_wrap {
    size_t       budget;
    int          first_indent;
    int          indent;
    const char  *gutter;
    const char *const *gutters;
    int          gutters_n;
    const char  *mark;
    enum ui_role role;
    const unsigned char *spans;
    int          max_rows;
    int          erase;
    int          paint_empty;
    int          measure;
    int          reflow_cols;
    int         *widths;
    int          widths_max;
};

int ui_wrap_paint(const char *text, const struct ui_wrap *w);

void ui_wrapped(const char *text, int indent, enum ui_role role);

__attribute__((format(printf, 2, 3))) void ui_bar(const char *style, const char *fmt, ...);

__attribute__((format(printf, 1, 2))) void ui_note(const char *fmt, ...);
__attribute__((format(printf, 1, 2))) void ui_error(const char *fmt, ...);

#endif
