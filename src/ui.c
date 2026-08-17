#include "ui.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#include "app.h"
#include "settings.h"
#include "tty.h"
#include "vendor/screen_color.h"
#include "vendor/colors.h"

static int use_color;
static int raw_newlines;

static const struct {
    const char *attr;
    int         slot;
    int         tint;
} ROLES[UI_RESET] = {
    [UI_ACCENT]  = { NULL, COLOR_BASE9, 0 },
    [UI_ECHO]    = { NULL, COLOR_BASE9, 90 },
    [UI_TEXT]    = { "39", -1, 0 },
    [UI_STICKY]  = { NULL, COLOR_BASE9, 90 },
    [UI_STICKY_DONE] = { NULL, COLOR_BASE11, 90 },
    [UI_BRAND]   = { NULL, COLOR_BASE12, 0 },
    [UI_CHROME]  = { NULL, COLOR_UI_BORDER_FLOAT, 0 },
    [UI_DIM]     = { NULL, COLOR_UI_DIM, 0 },
    [UI_BODY]    = { NULL, COLOR_BASE5, 0 },
    [UI_BOLD]    = { "1",  COLOR_BASE12, 0 },
    [UI_ITALIC]  = { "3",  COLOR_BASE12, 0 },
    [UI_CODE]    = { NULL, COLOR_BASE12, 0 },
    [UI_HEADING] = { "1",  COLOR_BASE12, 0 },
    [UI_LINK]    = { "4",  COLOR_BASE13, 0 },
    [UI_ERROR]   = { NULL, COLOR_UI_MSG_ERROR, 0 },
    [UI_OK]      = { NULL, COLOR_BASE11, 0 },
    [UI_THINKING] = { "3", COLOR_BASE14, 0 },
    [UI_TOOL]    = { NULL, COLOR_BASE12, 0 },
    [UI_SPIN]    = { NULL, COLOR_BASE12, 0 },
    [UI_BASH]    = { NULL, COLOR_BASE8, 90 },
};

static char styles[UI_RESET][64];

static int slots[UI_RESET];

static unsigned mix(unsigned fg, unsigned bg, int pct)
{
    return (fg * (100 - pct) + bg * pct + 50) / 100;
}

static void build_style(int role)
{
    const char *attr = ROLES[role].attr;
    if (slots[role] < 0) {
        snprintf(styles[role], sizeof styles[role], "\x1b[%sm", attr ? attr : "0");
        return;
    }
    Color c = color_get((ColorIndex)slots[role]);
    char wash[24] = "";
    if (ROLES[role].tint > 0) {
        Color b = color_get(COLOR_BASE0);
        snprintf(wash, sizeof wash, ";48;2;%u;%u;%u",
                 mix(c.r, b.r, ROLES[role].tint), mix(c.g, b.g, ROLES[role].tint),
                 mix(c.b, b.b, ROLES[role].tint));
    }
    snprintf(styles[role], sizeof styles[role], "\x1b[%s%s38;2;%u;%u;%u%sm",
             attr ? attr : "", attr ? ";" : "", c.r, c.g, c.b, wash);
}

static const struct {
    int         slot;
    const char *name;
} SWATCH[] = {

    {COLOR_BASE6,  "base6"},   {COLOR_BASE7,  "base7"},
    {COLOR_BASE8,  "red"},     {COLOR_BASE9,  "orange"},
    {COLOR_BASE10, "yellow"},  {COLOR_BASE11, "green"},
    {COLOR_BASE12, "lightblue"}, {COLOR_BASE13, "blue"},
    {COLOR_BASE14, "violet"},  {COLOR_BASE15, "magenta"},
};
#define SWATCH_N (COUNT(SWATCH))

static const struct {
    const char  *key;
    enum ui_role roles[8];
} GROUPS[] = {
    [UI_GROUP_INPUT]    = {SETTING_COLOR_INPUT,
                           {UI_ACCENT, UI_ECHO, UI_STICKY, UI_RESET}},
    [UI_GROUP_EMPHASIS] = {SETTING_COLOR_EMPHASIS,
                           {UI_BOLD, UI_ITALIC, UI_CODE, UI_HEADING, UI_SPIN,
                            UI_BRAND, UI_TOOL, UI_RESET}},
};
#define GROUP_N (COUNT(GROUPS))

static int cursor[GROUP_N];

static void apply_group(int group, int at)
{
    cursor[group] = at + 1;
    for (int i = 0; GROUPS[group].roles[i] != UI_RESET; i++) {
        int role = GROUPS[group].roles[i];
        slots[role] = SWATCH[at].slot;
        build_style(role);
    }
}

static int saved_swatch(int group)
{
    const char *name = settings_get_str(GROUPS[group].key, NULL);
    if (!name)
        return -1;
    for (int i = 0; i < SWATCH_N; i++)
        if (strcmp(SWATCH[i].name, name) == 0)
            return i;
    return -1;
}

const char *ui_cycle(enum ui_group group, int delta)
{
    static char label[64];

    if (!use_color || group < 0 || group >= GROUP_N)
        return "";
    if (!cursor[group]) {
        cursor[group] = 1;
        for (int i = 0; i < SWATCH_N; i++)
            if (SWATCH[i].slot == slots[GROUPS[group].roles[0]])
                cursor[group] = i + 1;
    }
    int at = (cursor[group] - 1 + delta % SWATCH_N + SWATCH_N) % SWATCH_N;
    apply_group(group, at);
    settings_set_str(GROUPS[group].key, SWATCH[at].name);

    Color c = color_get((ColorIndex)SWATCH[at].slot);
    snprintf(label, sizeof label, "%s #%02x%02x%02x (%d/%d)",
             SWATCH[at].name, c.r, c.g, c.b, at + 1, SWATCH_N);
    return label;
}

void ui_init(void)
{
    /* wcwidth() reports -1 for every non-ASCII codepoint in the startup "C"
       locale, which would make all width and wrapping math wrong. */
    setlocale(LC_CTYPE, "");

    const char *no_color = getenv("NO_COLOR");
    use_color = isatty(STDOUT_FILENO) && !(no_color && *no_color);
    if (!use_color)
        return;

    colors_init();
    for (int i = 0; i < UI_RESET; i++) {
        slots[i] = ROLES[i].slot;
        build_style(i);
    }

    for (int g = 0; g < GROUP_N; g++) {
        int at = saved_swatch(g);
        if (at >= 0)
            apply_group(g, at);
    }
}

const char *ui_style(enum ui_role role)
{
    if (!use_color)
        return "";
    if (role == UI_RESET)
        return "\x1b[0m";
    if (role < 0 || role >= UI_RESET)
        return "";
    return styles[role];
}

void ui_cursor_plain(void)
{
    if (!use_color)
        return;
    Color c = color_get(COLOR_UI_CURSOR_FG);
    printf("\x1b]12;#%02x%02x%02x\x07", c.r, c.g, c.b);
    fflush(stdout);
}

void ui_cursor_restore(void)
{
    if (!use_color)
        return;
    fputs("\x1b]112\x07", stdout);
    fflush(stdout);
}

void ui_raw(int on) { raw_newlines = on; }

/* Cells per line emitted since the mark, banked rather than turned into rows at
   emit time: a resize reflows everything already on screen, so how many rows
   that output occupies is only answerable at the width being asked about. Past
   SCROLL_LINES the answer is "further back than any screen is tall". */
#define SCROLL_LINES 512
static int scroll_widths[SCROLL_LINES];
static int scroll_count;
static int scroll_lost;
static int scroll_cells;
static int scroll_track = 1;

void ui_scroll_mark(void)
{
    scroll_count = 0;
    scroll_lost = 0;
    scroll_cells = 0;
}

int ui_scroll_rows(void)
{
    if (scroll_lost)
        return SCROLL_LINES;
    int cols = ui_columns();
    int rows = ui_reflow_rows(scroll_widths, scroll_count, cols);
    /* The terminal defers the wrap until a cell past the last column, so the
       line still being built has only scrolled once it passes that. */
    if (scroll_cells > 0 && cols > 0)
        rows += (scroll_cells - 1) / cols;
    return rows;
}

void ui_scroll_track(int on) { scroll_track = on ? 1 : 0; }

static void scroll_text(const char *s, size_t n)
{
    if (!scroll_track || !n)
        return;
    scroll_cells += (int)ui_cells_n(s, n);
}

static void scroll_linefeed(void)
{
    if (!scroll_track)
        return;
    if (scroll_count < SCROLL_LINES)
        scroll_widths[scroll_count++] = scroll_cells;
    else
        scroll_lost = 1;
    scroll_cells = 0;
}

static char  *capture;
static size_t capture_len, capture_cap;
static int    capture_on, capture_cols;

/* The one place every painter's bytes pass through, so a capture can stand in
   for the terminal without each of them knowing. */
static void emit(const char *s, size_t n)
{
    if (!capture_on) {
        fwrite(s, 1, n, stdout);
        return;
    }
    if (capture_len + n + 1 > capture_cap) {
        size_t cap = capture_cap ? capture_cap : 1024;
        while (cap < capture_len + n + 1)
            cap *= 2;
        char *grown = realloc(capture, cap);
        if (!grown)
            return;
        capture = grown;
        capture_cap = cap;
    }
    memcpy(capture + capture_len, s, n);
    capture_len += n;
    capture[capture_len] = '\0';
}

void ui_capture_begin(int columns)
{
    capture_len = 0;
    capture_cols = columns;
    capture_on = 1;
    if (capture)
        capture[0] = '\0';
}

char *ui_capture_end(void)
{
    capture_on = 0;
    capture_cols = 0;
    char *taken = capture_len ? strdup(capture) : NULL;
    capture_len = 0;
    return taken;
}

void ui_putn(const char *s, size_t n)
{
    if (capture_on) {
        emit(s, n);
        return;
    }
    if (!raw_newlines) {
        fwrite(s, 1, n, stdout);
        return;
    }
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\n')
            continue;
        fwrite(s + start, 1, i - start, stdout);
        fputs("\r\n", stdout);
        scroll_text(s + start, i - start);
        scroll_linefeed();
        start = i + 1;
    }
    if (start < n) {
        fwrite(s + start, 1, n - start, stdout);
        scroll_text(s + start, n - start);
    }
}

void ui_put(const char *s)
{
    if (s)
        ui_putn(s, strlen(s));
}

void ui_printf(const char *fmt, ...)
{
    char stack[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stack, sizeof stack, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof stack) {
        ui_putn(stack, (size_t)n);
        return;
    }
    char *heap = malloc((size_t)n + 1);
    if (!heap)
        return;
    va_start(ap, fmt);
    vsnprintf(heap, (size_t)n + 1, fmt, ap);
    va_end(ap);
    ui_putn(heap, (size_t)n);
    free(heap);
}

void ui_esc(const char *s)
{
    if (s)
        emit(s, strlen(s));
}

void ui_pad(int cells)
{
    for (int i = 0; i < cells; i++)
        emit(" ", 1);
}

void ui_move(int count, char direction)
{
    if (count <= 0)
        return;
    char esc[16];
    snprintf(esc, sizeof esc, "\x1b[%d%c", count, direction);
    ui_esc(esc);
}

void ui_sync_begin(void) { fputs("\x1b[?2026h", stdout); }
void ui_sync_end(void) { fputs("\x1b[?2026l", stdout); }

int ui_reflow_rows(const int *row_widths, int count, int cols)
{
    if (cols <= 0)
        return count > 0 ? count : 0;
    int rows = 0;
    for (int i = 0; i < count; i++)
        rows += row_widths[i] > 0 ? (row_widths[i] + cols - 1) / cols : 1;
    return rows;
}

void ui_flush(void) { fflush(stdout); }

int ui_columns(void) { return capture_cols > 0 ? capture_cols : tty_columns(); }

static unsigned decode(const char *s, size_t n, size_t *i)
{
    unsigned char b = (unsigned char)s[*i];
    unsigned cp;
    int extra;
    if (b < 0x80)      { (*i)++; return b; }
    else if (b < 0xC0) { (*i)++; return '?'; }
    else if (b < 0xE0) { cp = b & 0x1Fu; extra = 1; }
    else if (b < 0xF0) { cp = b & 0x0Fu; extra = 2; }
    else               { cp = b & 0x07u; extra = 3; }
    (*i)++;
    for (int k = 0; k < extra && *i < n; k++, (*i)++) {
        if (((unsigned char)s[*i] & 0xC0) != 0x80)
            return '?';
        cp = (cp << 6) | ((unsigned char)s[*i] & 0x3Fu);
    }
    return cp;
}

/* The symbol blocks are mostly narrow; only the code points that carry
   Emoji_Presentation render double-width, so they are listed rather than
   covered by a blanket range (which would mis-measure ✓, box drawing, …). */
static const struct {
    unsigned lo, hi;
} WIDE_SYMBOLS[] = {
    {0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3},
    {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F},
    {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE},
    {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA},
    {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD},
    {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C},
    {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797},
    {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
};

static int cell_width(unsigned cp)
{
    if (cp == 0)
        return 0;
    if (cp < 0x20 || cp == 0x7f)
        return 0;

    if ((cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x1F000 && cp <= 0x1F2FF))
        return 2;
    if (cp >= 0x231A && cp <= 0x27BF)
        for (int i = 0; i < COUNT(WIDE_SYMBOLS); i++)
            if (cp >= WIDE_SYMBOLS[i].lo && cp <= WIDE_SYMBOLS[i].hi)
                return 2;
    int w = wcwidth((wchar_t)cp);
    return w < 0 ? 1 : w;
}

size_t ui_cells_n(const char *s, size_t n)
{
    if (!s)
        return 0;
    size_t i = 0, cells = 0;
    while (i < n)
        cells += (size_t)cell_width(decode(s, n, &i));
    return cells;
}

size_t ui_cells(const char *s) { return s ? ui_cells_n(s, strlen(s)) : 0; }

/* Walks one escape sequence or one character, returning where it ends and
   adding what it costs on screen. */
static size_t step_visible(const char *s, size_t n, size_t i, size_t *cells)
{
    if (s[i] == '\x1b') {
        size_t j = i + 1;
        if (j < n && s[j] == '[') {
            for (j++; j < n && (s[j] < '@' || s[j] > '~'); j++)
                ;
        } else if (j < n && s[j] == ']') {
            for (j++; j < n && s[j] != '\a' && s[j] != '\x1b'; j++)
                ;
            if (j < n && s[j] == '\x1b')
                j++;
        }
        return j < n ? j + 1 : n;
    }
    size_t start = i++;
    while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80)
        i++;
    *cells += ui_cells_n(s + start, i - start);
    return i;
}

size_t ui_cells_visible(const char *s, size_t n)
{
    if (!s)
        return 0;
    size_t cells = 0, i = 0;
    while (i < n)
        i = step_visible(s, n, i, &cells);
    return cells;
}

size_t ui_fit_visible(const char *s, size_t n, size_t budget)
{
    if (!s)
        return 0;
    size_t cells = 0, i = 0, fit = 0;
    while (i < n) {
        size_t next = step_visible(s, n, i, &cells);
        if (cells > budget)
            break;
        i = fit = next;
    }
    return fit;
}

size_t ui_fit_bytes(const char *s, size_t budget)
{
    if (!s)
        return 0;
    size_t n = 0, fit = 0;
    while (s[n]) {
        n++;
        while (((unsigned char)s[n] & 0xC0) == 0x80)
            n++;
        if (ui_cells_n(s, n) > budget)
            break;
        fit = n;
    }
    return fit;
}

size_t ui_wrap_row(const char *s, size_t budget, size_t *skip)
{
    size_t n = strlen(s);
    *skip = 0;
    if (budget < 1)
        budget = 1;

    size_t i = 0, cells = 0, last_space = 0;
    while (i < n) {
        if (s[i] == '\n') {
            *skip = 1;
            return i;
        }
        size_t start = i;
        size_t w = (size_t)cell_width(decode(s, n, &i));
        if (cells + w > budget) {
            if (last_space > 0) {
                *skip = 1;
                return last_space;
            }
            /* A single glyph wider than the budget still has to be consumed,
               or the caller's `p += row + skip` walk never advances. */
            return start ? start : i;
        }
        cells += w;
        if (s[start] == ' ')
            last_space = start;
    }
    return n;
}

int ui_wrap_paint(const char *text, const struct ui_wrap *w)
{
    const char *p = text ? text : "";
    size_t budget = w->budget ? w->budget : 1;
    int mark_cells = w->mark ? (int)ui_cells(w->mark) : 0;
    int rows = 0;
    int reflowed = 0;
    int first = 1;

    while (*p || (first && w->paint_empty)) {
        size_t skip = 0;
        size_t row = *p ? ui_wrap_row(p, budget, &skip) : 0;
        int indent = first ? w->first_indent : w->indent;
        const char *gutter = w->gutter;
        if (w->gutters && rows < w->gutters_n && w->gutters[rows])
            gutter = w->gutters[rows];
        int gutter_cells = gutter ? (int)ui_cells(gutter) : 0;
        int width = gutter_cells + (first ? mark_cells : 0) + (int)ui_cells_n(p, row);

        if (!w->measure) {
            ui_pad(indent);
            if (w->role != UI_RESET)
                ui_esc(ui_style(w->role));
            if (w->erase)
                ui_esc(UI_ERASE_EOL);
            if (gutter)
                ui_put(gutter);
            if (w->mark && first)
                ui_put(w->mark);
            ui_putn(p, row);
        }

        p += row + skip;
        rows++;
        if (*p && w->max_rows && rows == w->max_rows) {
            if (!w->measure)
                ui_put("…");
            width++;
        }
        if (!w->measure) {
            if (w->role != UI_RESET)
                ui_esc(ui_style(UI_RESET));
            ui_put("\n");
        }
        if (w->widths && rows - 1 < w->widths_max)
            w->widths[rows - 1] = width + indent;
        if (w->reflow_cols > 0) {
            int cells = width + indent;
            reflowed += cells > 0 ? (cells + w->reflow_cols - 1) / w->reflow_cols : 1;
        }

        first = 0;
        if (w->max_rows && rows >= w->max_rows)
            break;
    }
    return w->reflow_cols > 0 ? reflowed : rows;
}

void ui_wrapped(const char *text, int indent, enum ui_role role)
{
    int columns = ui_columns();
    struct ui_wrap w = {0};
    w.budget = (size_t)(columns - indent > 1 ? columns - indent : 1);
    w.first_indent = w.indent = indent;
    w.role = role;
    w.paint_empty = 1;
    ui_wrap_paint(text, &w);
}

void ui_bar(const char *style, const char *fmt, ...)
{
    ui_esc(ui_style(UI_BRAND));
    ui_put(UI_BAR);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");
    if (style && *style)
        ui_esc(style);

    char stack[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(stack, sizeof stack, fmt, ap);
    va_end(ap);
    ui_put(stack);

    if (style && *style)
        ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static void ui_line(enum ui_role role, const char *fmt, va_list ap)
{
    char stack[1024];
    vsnprintf(stack, sizeof stack, fmt, ap);
    ui_esc(ui_style(role));
    ui_put(stack);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
    ui_flush();
}

void ui_note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ui_line(UI_DIM, fmt, ap);
    va_end(ap);
}

void ui_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ui_line(UI_ERROR, fmt, ap);
    va_end(ap);
}
