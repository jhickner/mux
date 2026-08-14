#include "banner.h"

#include "ui.h"

void banner_hints(void)
{
    ui_bar(ui_style(UI_DIM), "ctrl-d quit \xc2\xb7 try /help");
    ui_put("\n");
    ui_flush();
}
