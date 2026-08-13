#include "banner.h"

#include <stdio.h>
#include <string.h>

#include "session.h"
#include "ui.h"

#define APP_NAME "simple-agent"

/* Model ids are all "claude-"-prefixed here, so the prefix carries no
 * information in the banner. */
static const char *short_model(const char *model)
{
    if (strncmp(model, "claude-", 7) == 0 && model[7])
        return model + 7;
    return model;
}

void banner_identity(const struct session *s)
{
    char line[256];
    snprintf(line, sizeof line, "%s%s%s %s\xe2\x80\xba %s%s", ui_style(UI_BOLD), APP_NAME,
             ui_style(UI_RESET), ui_style(UI_DIM), short_model(session_model(s)),
             ui_style(UI_RESET));
    ui_bar("", "%s", line);
}

void banner_hints(void)
{
    ui_bar(ui_style(UI_DIM), "ctrl-d quit \xc2\xb7 try /help");
    ui_put("\n");
    ui_flush();
}

void banner_print(const struct session *s)
{
    banner_identity(s);
    banner_hints();
}
