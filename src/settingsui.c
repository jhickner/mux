#include "settingsui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "ask.h"
#include "image.h"
#include "muxcfg.h"
#include "pick.h"
#include "session.h"
#include "settings.h"
#include "status.h"
#include "text.h"
#include "ui.h"

enum kind { S_FLAG, S_PERMISSION, S_ROWS, S_COLOR, S_MATRIX };

struct entry {
    const char *name;
    const char *about;
    enum kind   kind;

    // S_FLAG: the two faces of it, off first.
    const char *off;
    const char *on;

    // S_ROWS: what the count may be, and what it means at zero.
    int         low;
    int         high;
    const char *unbounded;

    enum ui_group group;   /* S_COLOR */
};

static const struct entry ENTRIES[] = {
    {.name = "reasoning", .kind = S_FLAG,
     .about = "show what the model thinks on the way to an answer",
     .off = "hidden", .on = "shown"},
    {.name = "tool calls", .kind = S_FLAG,
     .about = "how much of each call is shown",
     .off = "full blocks with output", .on = "one row each"},
    {.name = "permission", .kind = S_PERMISSION,
     .about = "how the CLI gates tool calls"},
    {.name = "floating prompt", .kind = S_FLAG,
     .about = "keep what you typed above the spinner",
     .off = "off", .on = "on"},
    {.name = "image rows", .kind = S_ROWS,
     .about = "tallest an inline image may be drawn",
     .low = IMAGE_ROWS_MIN, .high = IMAGE_ROWS_MAX},
    {.name = "echoed rows", .kind = S_ROWS,
     .about = "how much of a long typed line is echoed back",
     .low = 0, .high = 100, .unbounded = "all of it"},
    {.name = "input colour", .kind = S_COLOR,
     .about = "the prompt, its echo, and the sticky line",
     .group = UI_GROUP_INPUT},
    {.name = "emphasis colour", .kind = S_COLOR,
     .about = "headings, bold, code, and the spinner",
     .group = UI_GROUP_EMPHASIS},
    {.name = "mux matrix", .kind = S_MATRIX,
     .about = "the backends /mux fans out over"},
};

static int flag_of(const struct session *s, int at)
{
    switch (at) {
    case 0:
        return session_thinking(s);
    case 1:
        return session_compact(s);
    default:
        return status_sticky_enabled();
    }
}

static void flag_set(struct session *s, int at, int on)
{
    switch (at) {
    case 0:
        session_set_thinking(s, on);
        settings_set_int(SETTING_THINKING, on);
        break;
    case 1:
        session_set_compact(s, on);
        settings_set_int(SETTING_COMPACT, on);
        break;
    default:
        status_sticky_set(on);
        settings_set_int(SETTING_STICKY, on);
        break;
    }
}

static int rows_of(const struct entry *e)
{
    return e->high == IMAGE_ROWS_MAX
               ? image_rows()
               : settings_get_int(SETTING_ECHO_ROWS, ECHO_ROWS_DEFAULT);
}

static void rows_set(const struct entry *e, int rows)
{
    if (e->high == IMAGE_ROWS_MAX) {
        image_set_rows(rows);
        settings_set_int(SETTING_IMAGE_ROWS, image_rows());
    } else {
        settings_set_int(SETTING_ECHO_ROWS, rows);
    }
}

static void value_of(const struct session *s, int at, char *out, size_t cap)
{
    const struct entry *e = &ENTRIES[at];

    switch (e->kind) {
    case S_FLAG:
        snprintf(out, cap, "%s", flag_of(s, at) ? e->on : e->off);
        break;
    case S_PERMISSION: {
        int index = session_permission_index(session_permission(s));
        snprintf(out, cap, "%s", session_permission_name(index < 0 ? 0 : index));
        break;
    }
    case S_ROWS: {
        int rows = rows_of(e);
        if (!rows && e->unbounded)
            snprintf(out, cap, "%s", e->unbounded);
        else
            snprintf(out, cap, "%d", rows);
        break;
    }
    case S_COLOR:
        snprintf(out, cap, "%s", ui_swatch(e->group));
        break;
    case S_MATRIX:
        snprintf(out, cap, "%s", muxcfg_active());
        break;
    }
}

static void edit_permission(struct session *s)
{
    struct pick_item items[8];
    int              count = session_permission_count();

    if (count > (int)COUNT(items))
        count = (int)COUNT(items);
    for (int i = 0; i < count; i++)
        items[i] = (struct pick_item){session_permission_name(i),
                                      session_permission_desc(i)};

    int now = session_permission_index(session_permission(s));
    int index = pick_run("gate tool calls", items, count, now < 0 ? 0 : now);
    if (index < 0 || index == now)
        return;

    if (session_set_permission(s, session_permission_name(index)))
        settings_set_int(SETTING_PERMISSION, index);
}

static void edit_rows(const struct entry *e)
{
    char title[160];
    snprintf(title, sizeof title, "%s \xc2\xb7 %d to %d", e->name, e->low, e->high);

    for (int tries = 0; tries < 3; tries++) {
        char now[16];
        snprintf(now, sizeof now, "%d", rows_of(e));

        char *text = ask_run(title, now);
        if (!text)
            return;

        text_chomp(text);
        char *end;
        long  rows = strtol(text, &end, 10);
        int   ok = end != text && !*end && rows >= e->low && rows <= e->high;
        free(text);

        if (ok) {
            rows_set(e, (int)rows);
            return;
        }
        snprintf(title, sizeof title, "%s \xe2\x80\x94 a number from %d to %d",
                 e->name, e->low, e->high);
    }
}

static void edit_color(const struct entry *e)
{
    const char *const *names = NULL;
    int                count = ui_swatches(&names);
    struct pick_item   items[24];

    if (count > (int)COUNT(items))
        count = (int)COUNT(items);

    const char *now = ui_swatch(e->group);
    int         initial = 0;
    for (int i = 0; i < count; i++) {
        items[i] = (struct pick_item){names[i], NULL};
        if (!strcmp(names[i], now))
            initial = i;
    }

    int index = pick_run(e->name, items, count, initial);
    if (index >= 0)
        ui_swatch_set(e->group, names[index]);
}

static void edit(struct session *s, int at)
{
    const struct entry *e = &ENTRIES[at];

    switch (e->kind) {
    case S_FLAG:
        flag_set(s, at, !flag_of(s, at));
        break;
    case S_PERMISSION:
        edit_permission(s);
        break;
    case S_ROWS:
        edit_rows(e);
        break;
    case S_COLOR:
        edit_color(e);
        break;
    case S_MATRIX:
        muxcfg_run();
        break;
    }
}

void settingsui_run(struct session *s)
{
    int sel = 0;

    for (;;) {
        char             labels[COUNT(ENTRIES)][96];
        struct pick_item items[COUNT(ENTRIES) + 1];
        int              count = 0;

        for (int i = 0; i < (int)COUNT(ENTRIES); i++) {
            char value[64];
            value_of(s, i, value, sizeof value);
            snprintf(labels[i], sizeof labels[i], "%-16s %s", ENTRIES[i].name, value);
            items[count++] = (struct pick_item){labels[i], ENTRIES[i].about};
        }
        items[count++] = (struct pick_item){"done", NULL};

        int index = pick_run(APP_NAME " settings", items, count, sel);
        if (index < 0 || index == count - 1)
            break;
        sel = index;
        edit(s, index);
    }
}
