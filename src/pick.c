#include "pick.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chrome.h"
#include "tty.h"
#include "ui.h"

#define VISIBLE_MAX 10

struct view {
    int top;
    int visible;
    const char *title;
    const struct pick_item *items;
    int n;          // every item offered
    int *order;     // the ones the query kept, best first
    int *score;
    int count;      // how many of order are live
    int sel;        // an index into order
    int filter;     // typing narrows the list instead of jumping through it
    char query[64];
};

// The same subsequence score files.c ranks path completions by.
static int fuzzy(const char *name, const char *q)
{
    int score = 0, ni = 0, streak = 0;

    for (int qi = 0; q[qi]; qi++) {
        int qc = tolower((unsigned char)q[qi]);
        int found = 0;
        while (name[ni]) {
            char prev = ni ? name[ni - 1] : '/';
            int nc = tolower((unsigned char)name[ni]);
            ni++;
            if (nc == qc) {
                streak++;
                score += 1 + streak;
                if (prev == '/' || prev == '-' || prev == '_' || prev == '.' || prev == ':')
                    score += 4;
                found = 1;
                break;
            }
            streak = 0;
        }
        if (!found)
            return -1;
    }
    return score;
}

// Rebuild the visible rows for the current query, keeping the highlight on the
// same item where the query still admits it.
static void refilter(struct view *v)
{
    int keep = (v->count && v->sel < v->count) ? v->order[v->sel] : -1;

    v->count = 0;
    for (int i = 0; i < v->n; i++) {
        int s = v->query[0] ? fuzzy(v->items[i].label, v->query) : 0;
        if (s < 0)
            continue;
        int at = v->count++;
        while (at > 0 && v->score[at - 1] < s) {
            v->order[at] = v->order[at - 1];
            v->score[at] = v->score[at - 1];
            at--;
        }
        v->order[at] = i;
        v->score[at] = s;
    }

    v->sel = 0;
    for (int i = 0; i < v->count; i++)
        if (v->order[i] == keep) {
            v->sel = i;
            break;
        }
    v->top = 0;
    v->visible = v->count < VISIBLE_MAX ? v->count : VISIBLE_MAX;
    if (v->visible < 1)
        v->visible = 1;
}

static int run(const char *title, const struct pick_item *items, int count,
               int initial, const char *shortcuts, int *pressed, int filter);

// A modal section: chrome.c paints it in place of the whole stack.
static void paint(void *ud)
{
    struct view *v = ud;
    const struct pick_item *items = v->items;
    int count = v->count, sel = v->sel;
    char title[192];

    if (v->filter && v->query[0])
        snprintf(title, sizeof title, "%s \xc2\xb7 %s", v->title, v->query);
    else if (v->filter)
        snprintf(title, sizeof title, "%s \xc2\xb7 type to filter", v->title);
    else
        snprintf(title, sizeof title, "%s", v->title);

    if (sel < v->top)
        v->top = sel;
    if (sel >= v->top + v->visible)
        v->top = sel - v->visible + 1;

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
        int selected = (row == sel);
        ui_esc(ui_style(selected ? UI_ACCENT : UI_RESET));
        ui_put(selected ? "  \xe2\x86\x92 " : "    ");

        size_t label_budget = columns > 5 ? (size_t)(columns - 5) : 1;
        size_t label_n = ui_fit_bytes(items[i].label, label_budget);
        ui_putn(items[i].label, label_n);
        if (items[i].label[label_n])
            ui_put("…");
        ui_esc(ui_style(UI_RESET));

        size_t used = 4 + ui_cells_n(items[i].label, label_n) +
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
    return run(title, items, count, initial, NULL, NULL, 0);
}

int pick_run_filter(const char *title, const struct pick_item *items, int count, int initial)
{
    return run(title, items, count, initial, NULL, NULL, 1);
}

int pick_run_keys(const char *title, const struct pick_item *items, int count,
                  int initial, const char *shortcuts, int *pressed)
{
    return run(title, items, count, initial, shortcuts, pressed, 0);
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
               int initial, const char *shortcuts, int *pressed, int filter)
{
    if (pressed)
        *pressed = 0;
    if (count <= 0)
        return -1;

    struct view v = {0};
    v.visible = count < VISIBLE_MAX ? count : VISIBLE_MAX;
    v.title = title;
    v.items = items;
    v.n = count;
    v.count = count;
    v.filter = filter;
    v.order = calloc((size_t)count, sizeof *v.order);
    v.score = calloc((size_t)count, sizeof *v.score);
    if (!v.order || !v.score) {
        free(v.order);
        free(v.score);
        return -1;
    }
    for (int i = 0; i < count; i++)
        v.order[i] = i;
    v.sel = (initial >= 0 && initial < count) ? initial : 0;
    // Nothing to pick with: a front end that is not the terminal asked, and
    // there is no keyboard to answer on. Reads as a cancel.
    if (!tty_is_raw())
        return -1;
    chrome_modal(paint, &v);

    int result = -1;
    for (;;) {
        tty_event ev;
        if (!tty_read(&ev, -1))
            continue;
        if (ev.key == TK_TEXT) {
            int took = filter && type_into(&v, ev.text, ev.text ? strlen(ev.text) : 0);
            free(ev.text);
            if (!took)
                continue;
            refilter(&v);
            chrome_paint();
            continue;
        }
        switch (ev.key) {
        case TK_UP:
            v.sel = v.sel > 0 ? v.sel - 1 : v.count - 1;
            break;
        case TK_DOWN:
            v.sel = v.sel + 1 < v.count ? v.sel + 1 : 0;
            break;
        case TK_HOME:
            v.sel = 0;
            break;
        case TK_END:
            v.sel = v.count - 1;
            break;
        case TK_BACKSPACE: {
            if (!filter || !v.query[0])
                break;
            size_t len = strlen(v.query);
            v.query[len - 1] = '\0';
            refilter(&v);
            break;
        }
        case TK_ENTER:
            if (!v.count)
                break;
            result = v.order[v.sel];
            goto done;
        case TK_ESCAPE:
            if (filter && v.query[0]) {
                v.query[0] = '\0';
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

            if (shortcuts && ev.cp > 0 && ev.cp < 128 &&
                strchr(shortcuts, (int)ev.cp)) {
                if (!v.count)
                    break;
                result = v.order[v.sel];
                if (pressed)
                    *pressed = (int)ev.cp;
                goto done;
            }

            if (filter) {
                char c = (char)ev.cp;
                if (ev.cp < 128 && type_into(&v, &c, 1))
                    refilter(&v);
                break;
            }
            if (ev.cp >= '1' && ev.cp <= '9' && (int)(ev.cp - '1') < v.count)
                v.sel = (int)(ev.cp - '1');
            break;
        case TK_RESIZE:
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
