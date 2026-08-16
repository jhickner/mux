
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

/* Named so the same escape is not spelled out across a dozen call sites. */
#define UI_CURSOR_SHOW  "\x1b[?25h"
#define UI_CURSOR_HIDE  "\x1b[?25l"
#define UI_ERASE_EOL    "\x1b[K"
#define UI_ERASE_BELOW  "\x1b[J"
#define UI_CLEAR_SCREEN "\x1b[2J\x1b[H"
#define UI_PASTE_ON     "\x1b[?2004h"
#define UI_PASTE_OFF    "\x1b[?2004l"

/* Paints the HUD rows for the given width and returns how many it drew. The
   idle (prompt) and busy (status) paths share this signature. */
typedef int (*ui_hud_fn)(void *ud, int cols);

void        ui_init(void);

/* Writes `cells` spaces. */
void ui_pad(int cells);

/* Emits a cursor-motion escape ('A' up, 'B' down, 'C' right, …). No-op when
   count <= 0, which every caller relies on. */
void ui_move(int count, char direction);
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

/* One description of a wrapped block, shared by every painter so that the code
   which paints a block and the code which later counts its rows cannot drift.
   Row layout is: pad, style on, erase, gutter, mark (first row), text,
   ellipsis (last row when clipped), style off, newline. */
struct ui_wrap {
    size_t       budget;
    int          first_indent; /* pad cells before the first row */
    int          indent;       /* pad cells before the rest */
    const char  *gutter;       /* drawn before the text on every row */
    const char  *mark;         /* drawn after the gutter, first row only */
    enum ui_role role;         /* UI_RESET leaves the text unstyled */
    int          max_rows;     /* 0 = unlimited; adds "…" when text remains */
    int          erase;        /* erase to end of line before each row */
    int          paint_empty;  /* empty text still yields one (blank) row */
    int          measure;      /* fill widths but paint nothing */
    int          reflow_cols;  /* non-zero: count rows as re-wrapped to this width */
    int         *widths;       /* optional out: painted cell width per row */
    int          widths_max;
};

/* Returns rows painted, or — when reflow_cols is set — how many terminal rows
   those painted rows would occupy at that width. */
int ui_wrap_paint(const char *text, const struct ui_wrap *w);

void ui_wrapped(const char *text, int indent, enum ui_role role);

__attribute__((format(printf, 2, 3))) void ui_bar(const char *style, const char *fmt, ...);

__attribute__((format(printf, 1, 2))) void ui_note(const char *fmt, ...);
__attribute__((format(printf, 1, 2))) void ui_error(const char *fmt, ...);

#endif
