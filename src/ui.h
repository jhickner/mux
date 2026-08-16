
#ifndef UI_H
#define UI_H

#include <stdarg.h>
#include <stddef.h>

enum ui_role {
    UI_ACCENT,
    UI_ECHO,
    UI_TEXT,
    UI_STICKY,
    UI_STICKY_DONE,
    UI_BRAND,
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
    UI_RESET,
};

enum ui_group {
    UI_GROUP_INPUT,
    UI_GROUP_EMPHASIS,
};

#define UI_BAR "\xe2\x96\x8c"

void        ui_init(void);
const char *ui_style(enum ui_role role);

const char *ui_cycle(enum ui_group group, int delta);

void ui_cursor_plain(void);
void ui_cursor_restore(void);

void ui_raw(int on);
void ui_put(const char *s);
void ui_putn(const char *s, size_t n);
__attribute__((format(printf, 1, 2))) void ui_printf(const char *fmt, ...);
void ui_flush(void);

void ui_esc(const char *s);

void ui_scroll_mark(void);
int  ui_scroll_rows(void);
void ui_scroll_track(int on);

void ui_sync_begin(void);
void ui_sync_end(void);

int ui_reflow_rows(const int *row_widths, int count, int cols);

int    ui_columns(void);
size_t ui_cells(const char *s);
size_t ui_cells_n(const char *s, size_t n);

size_t ui_wrap_row(const char *s, size_t budget, size_t *skip);

void ui_wrapped(const char *text, int indent, const char *style);

__attribute__((format(printf, 2, 3))) void ui_bar(const char *style, const char *fmt, ...);

__attribute__((format(printf, 1, 2))) void ui_note(const char *fmt, ...);
__attribute__((format(printf, 1, 2))) void ui_error(const char *fmt, ...);

#endif
