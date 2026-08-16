
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"
#include "md.h"
#include "ui.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <image> [more...]\n", argv[0]);
        return 2;
    }

    ui_init();
    image_init();
    printf("graphics: %s\n\n", image_available() ? "yes" : "no");

    char md[8192];
    size_t n = (size_t)snprintf(md, sizeof md, "Here is what came out:\n\n");
    for (int i = 1; i < argc && n < sizeof md; i++)
        n += (size_t)snprintf(md + n, sizeof md - n, "![render %d](%s)\n\n", i, argv[i]);
    snprintf(md + n, sizeof md - n, "That is **all** of them.\n");

    md_render(md, 0);
    ui_flush();
    return 0;
}
