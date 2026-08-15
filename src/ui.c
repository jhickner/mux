#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#include "settings.h"
#include "tty.h"
#include "vendor/screen_color.h"
#include "vendor/colors.h"

static int use_color;
static int raw_newlines;

/* Each role is an optional SGR attribute plus a foreground drawn from the
 * active colors.h theme. `slot` is -1 for the attribute-only roles. `tint` is
 * how far toward the background the role's own foreground is mixed to make a
 * background wash for it, in percent; 0 leaves the role foreground-only. */
static const struct {
    const char *attr;
    int         slot;
    int         tint;
} ROLES[UI_RESET] = {
    [UI_ACCENT]  = { NULL, COLOR_BASE9, 0 },
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
};

static char styles[UI_RESET][64];

/* The slot each role currently paints in. Seeded from ROLES; only the temporary
 * ui_cycle() below moves one. */
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

/* TEMP: keybound color try-out. Remove along with the Ctrl-N/Ctrl-O binds. */

static const struct {
    int         slot;
    const char *name;
} SWATCH[] = {
    /* Every stop is a distinct color: base5 is the body prose itself, and the
     * UI_TYPED / UI_PROMPT slots are references to base6 and base7, so all
     * three would be stops that look like no change at all. */
    {COLOR_BASE6,  "base6"},   {COLOR_BASE7,  "base7"},
    {COLOR_BASE8,  "red"},     {COLOR_BASE9,  "orange"},
    {COLOR_BASE10, "yellow"},  {COLOR_BASE11, "green"},
    {COLOR_BASE12, "lightblue"}, {COLOR_BASE13, "blue"},
    {COLOR_BASE14, "violet"},  {COLOR_BASE15, "magenta"},
};
#define SWATCH_N ((int)(sizeof SWATCH / sizeof *SWATCH))

/* The roles each keybind moves together, and the setting the pick is kept in.
 * A group ends at UI_RESET. */
static const struct {
    const char  *key;
    enum ui_role roles[8];
} GROUPS[] = {
    [UI_GROUP_INPUT]    = {SETTING_COLOR_INPUT,
                           {UI_ACCENT, UI_STICKY, UI_RESET}},
    [UI_GROUP_EMPHASIS] = {SETTING_COLOR_EMPHASIS,
                           {UI_BOLD, UI_ITALIC, UI_CODE, UI_HEADING, UI_SPIN,
                            UI_BRAND, UI_TOOL, UI_RESET}},
};
#define GROUP_N ((int)(sizeof GROUPS / sizeof *GROUPS))

static int cursor[GROUP_N]; /* the swatch each group sits on, 1-based; 0 unset */

static void apply_group(int group, int at)
{
    cursor[group] = at + 1;
    for (int i = 0; GROUPS[group].roles[i] != UI_RESET; i++) {
        int role = GROUPS[group].roles[i];
        slots[role] = SWATCH[at].slot;
        build_style(role);
    }
}

/* The saved pick for a group, or -1 when there is none and none is recognized. */
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
    const char *no_color = getenv("NO_COLOR");
    use_color = isatty(STDOUT_FILENO) && !(no_color && *no_color);
    if (!use_color)
        return;

    colors_init();
    for (int i = 0; i < UI_RESET; i++) {
        slots[i] = ROLES[i].slot;
        build_style(i);
    }
    /* TEMP: the cycled picks, if any were kept. */
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

static int scroll_rows;
static int scroll_col = 1; /* cells on the row being written, counted from 1 */
static int scroll_track = 1;

void ui_scroll_mark(void)
{
    scroll_rows = 0;
    scroll_col = 1;
}

int ui_scroll_rows(void) { return scroll_rows; }

void ui_scroll_track(int on) { scroll_track = on ? 1 : 0; }

/* A row that is filled exactly to the edge has not broken yet — the terminal
 * holds the wrap until something more is written — so the column is kept in
 * 1..cols and only what passes the edge counts as a row. */
static void scroll_text(const char *s, size_t n)
{
    if (!scroll_track || !n)
        return;
    int cols = tty_columns();
    scroll_col += (int)ui_cells_n(s, n);
    if (scroll_col > cols) {
        scroll_rows += (scroll_col - 1) / cols;
        scroll_col = (scroll_col - 1) % cols + 1;
    }
}

static void scroll_linefeed(void)
{
    if (!scroll_track)
        return;
    scroll_rows++;
    scroll_col = 1;
}

void ui_putn(const char *s, size_t n)
{
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
    fputs(s, stdout);
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

int ui_columns(void) { return tty_columns(); }

/* Decode one codepoint from s[*i .. n), advancing past it. */
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

static int cell_width(unsigned cp)
{
    if (cp == 0)
        return 0;
    if (cp < 0x20 || cp == 0x7f)
        return 0;
    /* Emoji and other wide pictographs that wcwidth() often misreports as 1. */
    if ((cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) ||
        (cp >= 0x1F000 && cp <= 0x1F2FF))
        return 2;
    int w = wcwidth((wchar_t)cp);
    return w < 0 ? 1 : w;
}

size_t ui_cells_n(const char *s, size_t n)
{
    size_t i = 0, cells = 0;
    while (i < n)
        cells += (size_t)cell_width(decode(s, n, &i));
    return cells;
}

size_t ui_cells(const char *s) { return s ? ui_cells_n(s, strlen(s)) : 0; }

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
            return start; /* one unbroken word: hard-break it */
        }
        cells += w;
        if (s[start] == ' ')
            last_space = start;
    }
    return n;
}

void ui_wrapped(const char *text, int indent, const char *style)
{
    int columns = ui_columns();
    size_t budget = (size_t)(columns - indent > 1 ? columns - indent : 1);
    const char *p = text;
    int first = 1;

    while (*p || first) {
        size_t skip = 0;
        size_t row = *p ? ui_wrap_row(p, budget, &skip) : 0;
        for (int k = 0; k < indent; k++)
            ui_put(" ");
        if (*style)
            ui_esc(style);
        ui_putn(p, row);
        if (*style)
            ui_esc(ui_style(UI_RESET));
        ui_put("\n");
        p += row + skip;
        first = 0;
    }
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
