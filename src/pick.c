#include "pick.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chrome.h"
#include "frontend.h"
#include "status.h"
#include "text.h"
#include "tty.h"
#include "ui.h"

struct view {
    int top;
    int visible;
    const char *title;
    const struct pick_item *items;
    const struct pick_live *live;
    const unsigned char *heading;
    int frame;
    double frame_at;
    int n;          // every item offered
    int *order;     // the ones the query kept, best first
    int *score;
    int count;      // how many of order are live
    int sel;        // an index into order
    int filter;     // typing narrows the list instead of jumping through it
    int slash;      // the query waits behind '/', so letters can be shortcuts
    int searching;  // '/' was pressed and the query is taking letters
    char query[64];
};

// How often a live list re-reads while nothing on it is spinning.
#define LIVE_POLL_MS 500

static const char *const SPIN[] = {"\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9",
                                  "\xe2\xa0\xb8", "\xe2\xa0\xbc", "\xe2\xa0\xb4",
                                  "\xe2\xa0\xa6", "\xe2\xa0\xa7", "\xe2\xa0\x87",
                                  "\xe2\xa0\x8f"};

static int item_heading(const struct view *v, int i)
{
    return v->heading && v->heading[i] == PICK_HEADING;
}

// A row that is still a row, but wants the blank line a heading would get.
static int item_apart(const struct view *v, int i)
{
    return v->heading && v->heading[i] == PICK_APART;
}

static int item_spins(const struct view *v, int i)
{
    return v->live && v->live->spin && v->live->spin[i];
}

// Anything turning on screen, so the wait for a key knows to end on a frame.
static int animating(const struct view *v)
{
    for (int i = 0; i < v->count; i++)
        if (item_spins(v, v->order[i]))
            return 1;
    return 0;
}

static int row_heading(const struct view *v, int row)
{
    return item_heading(v, v->order[row]);
}

static int row_apart(const struct view *v, int row)
{
    return item_apart(v, v->order[row]);
}

// Headings are passed over: the highlight only ever rests on something that
// can be chosen.
static void settle(struct view *v, int dir)
{
    for (int i = 0; i < v->count; i++) {
        if (!row_heading(v, v->sel))
            return;
        v->sel += dir;
        if (v->sel >= v->count)
            v->sel = 0;
        else if (v->sel < 0)
            v->sel = v->count - 1;
    }
}

static void step(struct view *v, int dir)
{
    if (!v->count)
        return;
    v->sel += dir;
    if (v->sel >= v->count)
        v->sel = 0;
    else if (v->sel < 0)
        v->sel = v->count - 1;
    settle(v, dir);
}

// As many rows as the window has room for, once the title, the count line and
// every blank line that sets a row off from the one above it are taken out.
static int visible_cap(const struct view *v)
{
    int rows = tty_rows() - 3 - chrome_gap();
    if (v->heading) {
        int breaks = 0;
        for (int i = 0; i < v->count; i++)
            if (row_heading(v, i) || row_apart(v, i))
                breaks++;
        if (breaks > 1)
            rows -= breaks - 1;
    }
    return rows < 5 ? 5 : rows;
}

// Rebuild the visible rows for the current query, keeping the highlight on the
// same item where the query still admits it.
static void refilter(struct view *v)
{
    int keep = (v->count && v->sel < v->count) ? v->order[v->sel] : -1;
    int under = 0;      // the query names the heading these rows sit under

    v->count = 0;
    for (int i = 0; i < v->n; i++) {
        // A grouped list keeps its order, so every row stays under the
        // heading that says where it lives. A heading matches for everything
        // beneath it -- typing part of a directory keeps that whole group --
        // and one whose group the query emptied goes with it.
        if (item_heading(v, i)) {
            under = !v->query[0] || text_fuzzy_score(v->items[i].label, v->query) >= 0;
            int has = under;
            for (int j = i + 1; !has && j < v->n && !item_heading(v, j); j++)
                if (text_fuzzy_score(v->items[j].label, v->query) >= 0)
                    has = 1;
            if (!has)
                continue;
            v->order[v->count] = i;
            v->score[v->count++] = 0;
            continue;
        }
        int s = v->query[0] ? text_fuzzy_score(v->items[i].label, v->query) : 0;
        if (s < 0 && !under)
            continue;
        int at = v->count++;
        // Ungrouped lists rank the best match first instead.
        while (!v->heading && at > 0 && v->score[at - 1] < s) {
            v->order[at] = v->order[at - 1];
            v->score[at] = v->score[at - 1];
            at--;
        }
        v->order[at] = i;
        v->score[at] = s < 0 ? 0 : s;
    }

    v->sel = 0;
    for (int i = 0; i < v->count; i++)
        if (v->order[i] == keep) {
            v->sel = i;
            break;
        }
    if (v->count)
        settle(v, 1);
    v->top = 0;
    int cap = visible_cap(v);
    v->visible = v->count < cap ? v->count : cap;
    if (v->visible < 1)
        v->visible = 1;
}

static int run(const char *title, const struct pick_item *items, int count,
               int initial, const struct pick_live *live, const char *shortcuts,
               int *pressed, int filter, int slash);

// A modal section: chrome.c paints it in place of the whole stack.
static void paint(void *ud)
{
    struct view *v = ud;
    const struct pick_item *items = v->items;
    int count = v->count, sel = v->sel;
    char title[192];

    if (v->filter && (v->searching || v->query[0])) {
        // The keys the list offers step aside while it is being searched: what
        // the query says is the only part that is changing.
        const char *rest = strstr(v->title, " \xc2\xb7 ");
        int lead = rest ? (int)(rest - v->title) : (int)strlen(v->title);
        snprintf(title, sizeof title, "%.*s \xc2\xb7 /%s", lead, v->title, v->query);
    }
    else if (v->filter && !v->slash)
        snprintf(title, sizeof title, "%s \xc2\xb7 type to filter", v->title);
    else
        snprintf(title, sizeof title, "%s", v->title);

    if (sel < v->top)
        v->top = sel;
    if (sel >= v->top + v->visible)
        v->top = sel - v->visible + 1;
    if (v->top < 0)
        v->top = 0;

    int columns = ui_columns();
    int rows = 0;

    ui_esc(ui_style(UI_CHROME));
    ui_put(UI_BAR);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");
    ui_esc(ui_style(UI_DIM));
    {
        size_t title_budget = columns > 3 ? (size_t)(columns - 3) : 1;
        size_t title_n = ui_fit_bytes(title, title_budget);
        ui_putn(title, title_n);
        if (title[title_n])
            ui_put("…");
    }
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
    rows++;

    int end = v->top + v->visible;
    if (end > count)
        end = count;
    for (int row = v->top; row < end; row++) {
        int i = v->order ? v->order[row] : row;
        if (item_heading(v, i)) {
            // A blank line sets each group off from the one above it.
            if (row > v->top) {
                ui_put("\n");
                rows++;
            }
            ui_esc(ui_style(UI_DIM));
            ui_put("  ");
            size_t budget = columns > 3 ? (size_t)(columns - 3) : 1;
            size_t fit = ui_fit_bytes(items[i].label, budget);
            ui_putn(items[i].label, fit);
            if (items[i].label[fit])
                ui_put("…");
            ui_esc(ui_style(UI_RESET));
            ui_put("\n");
            rows++;
            continue;
        }
        // Set off from the group above it without being a heading of its own.
        if (row_apart(v, row) && row > v->top) {
            ui_put("\n");
            rows++;
        }
        int selected = (row == sel);
        ui_esc(ui_style(selected ? UI_ACCENT : UI_RESET));
        ui_put(selected ? "  \xe2\x86\x92 " : "    ");

        // The status column: what the row is doing, ahead of what it is.
        size_t status = 0;
        if (v->live && (v->live->spin || v->live->mark)) {
            const char *mark = v->live->mark ? v->live->mark[i] : NULL;
            if (item_spins(v, i)) {
                ui_esc(ui_style(UI_SPIN));
                ui_put(SPIN[v->frame % (int)(sizeof SPIN / sizeof *SPIN)]);
                ui_esc(ui_style(selected ? UI_ACCENT : UI_RESET));
                ui_put(" ");
            } else if (mark && *mark) {
                ui_esc(ui_style(v->live->mark_role
                                ? (enum ui_role)v->live->mark_role[i]
                                : UI_ERROR));
                ui_put(mark);
                ui_esc(ui_style(selected ? UI_ACCENT : UI_RESET));
                ui_put(" ");
            } else {
                ui_put("  ");
            }
            status = 2;
        }

        size_t label_budget = columns > 5 + (int)status ? (size_t)(columns - 5 - (int)status) : 1;
        size_t label_n = ui_fit_bytes(items[i].label, label_budget);
        ui_putn(items[i].label, label_n);
        if (items[i].label[label_n])
            ui_put("…");
        ui_esc(ui_style(UI_RESET));

        size_t used = 4 + status + ui_cells_n(items[i].label, label_n) +
                      (items[i].label[label_n] ? 1 : 0);

        if (items[i].detail && *items[i].detail) {
            int budget = columns - (int)used - 4;
            if (budget > 6) {
                ui_put("  ");
                ui_esc(ui_style(UI_DIM));
                size_t skip = 0;
                size_t fit = ui_wrap_row(items[i].detail, strlen(items[i].detail),
                                         (size_t)budget, &skip, NULL);
                ui_putn(items[i].detail, fit);
                if (items[i].detail[fit])
                    ui_put("…");
                ui_esc(ui_style(UI_RESET));
            }
        }
        ui_put("\n");
        rows++;
    }

    if (!count) {
        ui_esc(ui_style(UI_DIM));
        ui_put("    no match");
        ui_esc(ui_style(UI_RESET));
        ui_put("\n");
        rows++;
    }

    if (count > v->visible) {
        ui_esc(ui_style(UI_DIM));
        ui_printf("    %d\u2013%d of %d", v->top + 1, end, count);
        ui_esc(ui_style(UI_RESET));
        ui_put("\n");
        rows++;
    }

    (void)rows;
}

int pick_run(const char *title, const struct pick_item *items, int count, int initial)
{
    return run(title, items, count, initial, NULL, NULL, NULL, 0, 0);
}

int pick_run_filter(const char *title, const struct pick_item *items, int count, int initial)
{
    return run(title, items, count, initial, NULL, NULL, NULL, 1, 0);
}

int pick_run_live(const char *title, const struct pick_item *items, int count,
                  int initial, const struct pick_live *live,
                  enum pick_search search, const char *shortcuts, int *pressed)
{
    return run(title, items, count, initial, live, shortcuts, pressed, 1,
               search == PICK_SEARCH_SLASH);
}

int pick_run_keys(const char *title, const struct pick_item *items, int count,
                  int initial, const char *shortcuts, int *pressed)
{
    return run(title, items, count, initial, NULL, shortcuts, pressed, 0, 0);
}

// Typed bytes go to the query in filter mode; everywhere else they are
// shortcuts and jump-to-row digits.
static int type_into(struct view *v, const char *s, size_t n)
{
    size_t len = strlen(v->query);
    int    took = 0;

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f || len + 1 >= sizeof v->query)
            continue;
        v->query[len++] = (char)c;
        took = 1;
    }
    v->query[len] = '\0';
    return took;
}

static int run(const char *title, const struct pick_item *items, int count,
               int initial, const struct pick_live *live, const char *shortcuts,
               int *pressed, int filter, int slash)
{
    if (pressed)
        *pressed = 0;
    if (count <= 0)
        return -1;

    // Nothing to pick with: a front end that is not the terminal asked, or
    // the terminal is not taking keys. Reads as a cancel.
    if (!frontend_has_keyboard() || !tty_is_raw())
        return -1;

    struct view v = {0};
    v.title = title;
    v.items = items;
    v.live = live;
    v.heading = live ? live->heading : NULL;
    v.n = count;
    v.filter = filter;
    v.slash = slash;
    v.order = calloc((size_t)count, sizeof *v.order);
    v.score = calloc((size_t)count, sizeof *v.score);
    if (!v.order || !v.score) {
        free(v.order);
        free(v.score);
        return -1;
    }
    refilter(&v);
    for (int i = 0; i < v.count; i++)
        if (v.order[i] == initial) {
            v.sel = i;
            break;
        }
    if (v.count)
        settle(&v, 1);
    chrome_modal(paint, &v);

    int result = -1;
    for (;;) {
        tty_event ev;
        int turning = animating(&v);
        // Nothing is turning, but a list that watches other windows still has
        // to hear about a session that starts working while it sits idle.
        int watching = v.live && v.live->tick;
        int wait = turning ? SPIN_FRAME_MS : (watching ? LIVE_POLL_MS : -1);
        if (!tty_read(&ev, wait)) {
            if (chrome_modal_interrupted())
                goto done;
            if (!turning && !watching)
                continue;
            // A frame of the spinners, and a chance for the caller to say the
            // rows have moved on.
            int moved = watching && v.live->tick(v.live->ud);
            if (moved)
                refilter(&v);
            if ((turning && spin_advance(&v.frame, &v.frame_at)) || moved)
                chrome_paint();
            continue;
        }
        int typing = filter && (!slash || v.searching);
        if (ev.key == TK_TEXT) {
            int took = typing && type_into(&v, ev.text, ev.text ? strlen(ev.text) : 0);
            free(ev.text);
            if (!took)
                continue;
            refilter(&v);
            chrome_paint();
            continue;
        }
        switch (ev.key) {
        case TK_UP:
            step(&v, -1);
            break;
        case TK_DOWN:
            step(&v, 1);
            break;
        case TK_HOME:
            if (!v.count)
                break;
            v.sel = 0;
            settle(&v, 1);
            break;
        case TK_END:
            if (!v.count)
                break;
            v.sel = v.count - 1;
            settle(&v, -1);
            break;
        case TK_BACKSPACE: {
            if (!typing)
                break;
            if (!v.query[0]) {
                // Backspacing out of an empty query leaves the search.
                v.searching = 0;
                break;
            }
            size_t len = strlen(v.query);
            v.query[len - 1] = '\0';
            refilter(&v);
            break;
        }
        case TK_RIGHT:
            // Right takes the highlighted row, the same as enter. A caller
            // that asked for it as a shortcut of its own is told which of the
            // two was pressed; the rest cannot tell them apart.
            if (!v.count || row_heading(&v, v.sel))
                break;
            result = v.order[v.sel];
            if (pressed && shortcuts && strchr(shortcuts, PICK_KEY_RIGHT))
                *pressed = PICK_KEY_RIGHT;
            goto done;
        case TK_NEWLINE:
            // Shift-enter, where the terminal reports it. Only a caller that
            // asked for it hears about it.
            if (!shortcuts || !strchr(shortcuts, '\n'))
                continue;
            if (!v.count || row_heading(&v, v.sel))
                break;
            result = v.order[v.sel];
            if (pressed)
                *pressed = '\n';
            goto done;
        case TK_ENTER:
            if (!v.count || row_heading(&v, v.sel))
                break;
            result = v.order[v.sel];
            goto done;
        case TK_ESCAPE:
            if (filter && (v.searching || v.query[0])) {
                v.query[0] = '\0';
                v.searching = 0;
                refilter(&v);
                break;
            }
            goto done;
        case TK_EOF:
            goto done;
        case TK_CHAR:
            if (ev.cp == 3 || ev.cp == 4)
                goto done;
            if (filter && ev.cp == 21) {   /* ctrl-u clears the query */
                v.query[0] = '\0';
                refilter(&v);
                break;
            }
            if (filter && slash && !v.searching && ev.cp == '/') {
                v.searching = 1;
                break;
            }

            // While a query is taking letters only the control keys still
            // reach the shortcuts: a letter means itself.
            if (shortcuts && ev.cp > 0 && ev.cp < 128 && !(typing && ev.cp >= 0x20) &&
                strchr(shortcuts, (int)ev.cp)) {
                if (!v.count || row_heading(&v, v.sel))
                    break;
                result = v.order[v.sel];
                if (pressed)
                    *pressed = (int)ev.cp;
                goto done;
            }

            if (typing) {
                char c = (char)ev.cp;
                if (ev.cp < 128 && type_into(&v, &c, 1))
                    refilter(&v);
                break;
            }
            if (ev.cp >= '1' && ev.cp <= '9' && (int)(ev.cp - '1') < v.count) {
                v.sel = (int)(ev.cp - '1');
                settle(&v, 1);
            }
            break;
        case TK_RESIZE:
            refilter(&v);
            break;
        default:
            continue;
        }
        chrome_paint();
    }

done:
    chrome_modal(NULL, NULL);
    free(v.order);
    free(v.score);
    return result;
}
