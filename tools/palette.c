
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "screen_color.h"
#define COLORS_IMPLEMENTATION
#include "colors.h"

static const char *SLOT_NAMES[COLOR_COUNT] = {
    "BASE0", "BASE1", "BASE2", "BASE3",
    "BASE4", "BASE5", "BASE6", "BASE7",
    "BASE8 (red)", "BASE9 (orange)", "BASE10 (yellow)", "BASE11 (green)",
    "BASE12 (lt blue)", "BASE13 (blue)", "BASE14 (violet)", "BASE15 (magenta)",

    "UI_MSG_SYSTEM", "UI_MSG_PROMPT", "UI_MSG_ITEM", "UI_MSG_USER",
    "UI_MSG_ERROR", "UI_MSG_DEFAULT", "UI_MSG_DESC", "UI_PROMPT",
    "UI_CURSOR_FG", "UI_CURSOR_BG", "UI_DIM", "UI_TYPED",
    "UI_BORDER", "UI_BORDER_FLOAT",

    "TORCH", "BLOOD", "DEAD", "NEON_PURPLE",
    "NONE", "MUZZLE_FLASH", "DEBUG",
};

static const char *ESC = "\x1b";

static void print_row(ColorIndex idx) {
    Color c = color_get(idx);
    const ColorEntry *e = &g_palette[idx];

    char ref[32] = "";
    if (e->is_ref) {
        snprintf(ref, sizeof ref, "-> %s", SLOT_NAMES[e->ref]);
    }

    printf("  %s[48;2;%d;%d;%dm        %s[0m", ESC, c.r, c.g, c.b, ESC);
    Color bg = color_get(COLOR_BASE0);
    printf(" %s[48;2;%d;%d;%dm%s[38;2;%d;%d;%dm Agg %s[0m",
           ESC, bg.r, bg.g, bg.b, ESC, c.r, c.g, c.b, ESC);
    printf("  #%02x%02x%02x  %-17s %s\n", c.r, c.g, c.b, SLOT_NAMES[idx], ref);
}

static void print_theme(int t) {
    colors_set_theme(t);
    printf("\n%s[1m%d. %s%s[0m\n", ESC, t, colors_theme_name(t), ESC);

    printf("\n  base ramp\n");
    for (int i = COLOR_BASE0; i <= COLOR_BASE7; i++) print_row(i);

    printf("\n  accents\n");
    for (int i = COLOR_BASE8; i <= COLOR_BASE15; i++) print_row(i);

    printf("\n  ui\n");
    for (int i = COLOR_UI_MSG_SYSTEM; i <= COLOR_UI_BORDER_FLOAT; i++) print_row(i);

    printf("\n  effects\n");
    for (int i = COLOR_TORCH; i <= COLOR_DEBUG; i++) print_row(i);
}

int main(int argc, char **argv) {
    colors_init();

    if (argc > 1 && strcmp(argv[1], "--esc") == 0) {
        ESC = "\\e";
        argc--;
        argv++;
    }

    if (argc > 1) {
        char *end;
        long n = strtol(argv[1], &end, 10);
        int is_num = (*argv[1] && !*end);
        for (int t = 0; t < colors_theme_count(); t++) {
            if (strcmp(argv[1], colors_theme_name(t)) == 0 || (is_num && n == t)) {
                print_theme(t);
                return 0;
            }
        }
        fprintf(stderr, "unknown theme: %s\n", argv[1]);
        return 1;
    }

    for (int t = 0; t < colors_theme_count(); t++) print_theme(t);
    return 0;
}
