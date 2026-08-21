#include "ask.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chrome.h"
#include "text.h"
#include "tty.h"
#include "ui.h"

#define ASK_MAX 1024

struct field {
    const char *title;
    char        text[ASK_MAX];
    size_t      len;
    size_t      at;   /* the caret, as a byte offset into text */
};

static int lead_byte(const char *s, size_t at)
{
    return ((unsigned char)s[at] & 0xC0) != 0x80;
}

static size_t step_left(const struct field *f, size_t at)
{
    while (at > 0 && !lead_byte(f->text, --at))
        ;
    return at;
}

static size_t step_right(const struct field *f, size_t at)
{
    while (at < f->len && !lead_byte(f->text, ++at))
        ;
    return at;
}

static void cut(struct field *f, size_t from, size_t to)
{
    memmove(f->text + from, f->text + to, f->len - to);
    f->len -= to - from;
    f->text[f->len] = '\0';
    f->at = from;
}

static void insert(struct field *f, const char *s, size_t n)
{
    if (!n || f->len + n >= sizeof f->text)
        return;
    memmove(f->text + f->at + n, f->text + f->at, f->len - f->at);
    memcpy(f->text + f->at, s, n);
    f->len += n;
    f->at += n;
    f->text[f->len] = '\0';
}

static void insert_codepoint(struct field *f, uint32_t cp)
{
    char buf[4];
    insert(f, buf, text_utf8_encode(cp, buf));
}

// The line scrolls under a fixed caret rather than wrapping: everything here
// is one field on one row.
static void paint(void *ud)
{
    struct field *f = ud;
    int           columns = ui_columns();
    int           budget = columns > 8 ? columns - 4 : 4;

    ui_esc(ui_style(UI_CHROME));
    ui_put(UI_BAR);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");
    ui_esc(ui_style(UI_DIM));
    ui_putn(f->title, ui_fit_bytes(f->title, (size_t)(columns > 3 ? columns - 3 : 1)));
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");

    size_t from = 0;
    while (ui_cells_n(f->text + from, f->at - from) > (size_t)budget - 1)
        from = step_right(f, from);

    ui_put("  ");
    ui_esc(ui_style(UI_TEXT));
    ui_putn(f->text + from, f->at - from);

    // The terminal's own caret sits with the prompt, so this one is drawn.
    size_t next = step_right(f, f->at);
    ui_esc("\x1b[7m");
    if (next > f->at)
        ui_putn(f->text + f->at, next - f->at);
    else
        ui_put(" ");
    ui_esc("\x1b[27m");

    if (next < f->len)
        ui_putn(f->text + next, ui_fit_bytes(f->text + next, (size_t)budget));
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");

    ui_esc(ui_style(UI_DIM));
    ui_put("  enter to keep, esc to leave it alone");
    ui_esc(ui_style(UI_RESET));
}

char *ask_run(const char *title, const char *initial)
{
    if (!tty_is_raw())
        return NULL;            // nothing to type on

    struct field f = {.title = title ? title : ""};
    if (initial) {
        snprintf(f.text, sizeof f.text, "%s", initial);
        f.len = f.at = strlen(f.text);
    }

    chrome_modal(paint, &f);
    for (;;) {
        tty_event ev;
        if (!tty_read(&ev, -1)) {
            if (!chrome_modal_interrupted())
                continue;
            chrome_modal(NULL, NULL);
            return NULL;
        }

        switch (ev.key) {
        case TK_TEXT:
            // A paste is one line: newlines would break out of the field.
            for (char *p = ev.text; p && *p; p++)
                if (*p == '\n' || *p == '\r')
                    *p = ' ';
            insert(&f, ev.text, ev.text ? strlen(ev.text) : 0);
            free(ev.text);
            break;
        case TK_CHAR:
            if (ev.cp == 3 || ev.cp == 4) {
                chrome_modal(NULL, NULL);
                return NULL;
            }
            if (ev.cp == 21) {          /* ctrl-u */
                cut(&f, 0, f.at);
            } else if (ev.cp == 23) {   /* ctrl-w */
                size_t to = f.at;
                while (f.at && f.text[f.at - 1] == ' ')
                    f.at--;
                while (f.at && f.text[f.at - 1] != ' ')
                    f.at--;
                cut(&f, f.at, to);
            } else if (ev.cp >= 0x20) {
                insert_codepoint(&f, ev.cp);
            }
            break;
        case TK_BACKSPACE:
            if (f.at)
                cut(&f, step_left(&f, f.at), f.at);
            break;
        case TK_DELETE:
            if (f.at < f.len)
                cut(&f, f.at, step_right(&f, f.at));
            break;
        case TK_LEFT:
            f.at = step_left(&f, f.at);
            break;
        case TK_RIGHT:
            f.at = step_right(&f, f.at);
            break;
        case TK_HOME:
            f.at = 0;
            break;
        case TK_END:
            f.at = f.len;
            break;
        case TK_ENTER: {
            chrome_modal(NULL, NULL);
            return strdup(f.text);
        }
        case TK_ESCAPE:
        case TK_EOF:
            chrome_modal(NULL, NULL);
            return NULL;
        default:
            break;
        }
        chrome_paint();
    }
}
