/**
 * colors.h - Runtime-switchable color themes (single-header, stb-style)
 *
 * A base16-style semantic palette with runtime theme switching:
 * - 16 base colors (0-7: background→foreground ramp, 8-15: accents)
 * - Semantic UI slots (message log, prompt, cursor, borders, ...)
 * - Effect colors (torch, blood, muzzle flash, ...)
 *
 * Palette entries are either a direct RGB value or a *reference* to another
 * index, so a theme can point its UI/effect slots at its own base ramp and a
 * single theme swap recolors everything. References are resolved on lookup.
 *
 * Ships with 9 themes: default, high-contrast, solarized (dark/light),
 * gruvbox, molokai, tokusa, koda.
 *
 * SINGLE-HEADER, stb-style. In exactly ONE .c file:
 *
 *     #define COLORS_IMPLEMENTATION
 *     #include "colors.h"
 *
 * and include "colors.h" plainly wherever else you need the declarations.
 *
 * Depends on the sibling screen.h library only for its 24-bit `Color` type
 * ({uint8_t r,g,b}). Build against -I../screen. If you already have a
 * compatible `Color` in scope you can include that header before this one; the
 * screen.h include is guarded so it won't double-include.
 */

#ifndef COLORS_H
#define COLORS_H

#include <stdbool.h>

// The 24-bit `Color` type comes from the sibling screen library. Guarded so a
// TU that already pulled in screen.h (e.g. to define SCREEN_IMPLEMENTATION)
// doesn't re-include it.
#ifndef SCREEN_H
#include "screen.h"
#endif

// Palette slot indices. Order matters: it is the layout of every theme's
// `palette` array, so keep base/accents first, then UI, then effects.
typedef enum {
    COLOR_BASE0 = 0,
    COLOR_BASE1,
    COLOR_BASE2,
    COLOR_BASE3,
    COLOR_BASE4,
    COLOR_BASE5,
    COLOR_BASE6,
    COLOR_BASE7,
    COLOR_BASE8,    // red
    COLOR_BASE9,    // orange
    COLOR_BASE10,   // yellow
    COLOR_BASE11,   // green
    COLOR_BASE12,   // light blue
    COLOR_BASE13,   // blue
    COLOR_BASE14,   // violet
    COLOR_BASE15,   // magenta

    // UI colors
    COLOR_UI_MSG_SYSTEM,    // Message log: system messages
    COLOR_UI_MSG_PROMPT,    // Message log: prompts
    COLOR_UI_MSG_ITEM,      // Message log: item names
    COLOR_UI_MSG_USER,      // Message log: user input
    COLOR_UI_MSG_ERROR,     // Message log: errors
    COLOR_UI_MSG_DEFAULT,   // Message log: default text
    COLOR_UI_MSG_DESC,      // Message log: item descriptions
    COLOR_UI_PROMPT,        // Input prompt text
    COLOR_UI_CURSOR_FG,     // Cursor foreground
    COLOR_UI_CURSOR_BG,     // Cursor background
    COLOR_UI_DIM,           // Dimmed/placeholder text
    COLOR_UI_TYPED,         // Typed input text
    COLOR_UI_BORDER,        // Panel borders
    COLOR_UI_BORDER_FLOAT,  // Floating panel borders

    // Effects
    COLOR_TORCH,
    COLOR_BLOOD,
    COLOR_DEAD,
    COLOR_NEON_PURPLE,
    COLOR_NONE,
    COLOR_MUZZLE_FLASH,     // Gunfire flash
    COLOR_DEBUG,            // Debug/unknown indicators

    COLOR_COUNT
} ColorIndex;

// A palette entry: either a direct RGB value or a reference to another index.
typedef struct {
    bool is_ref;
    union {
        Color color;
        ColorIndex ref;
    };
} ColorEntry;

// Helper macros for palette initialization.
#define COLOR_RGB(r, g, b)  { .is_ref = false, .color = { (r), (g), (b) } }
#define COLOR_REF(idx)      { .is_ref = true, .ref = (idx) }

// A named, complete palette.
typedef struct {
    const char *name;
    ColorEntry palette[COLOR_COUNT];
} ColorTheme;

// Active palette (mutable, copied from the selected theme on switch).
extern ColorEntry g_palette[COLOR_COUNT];

// Initialize the color system with the default theme (index 0).
void colors_init(void);

// Resolve a slot to its final RGB value (following references).
Color color_get(ColorIndex idx);

// Theme management.
void colors_set_theme(int theme_idx);
int colors_get_theme(void);
int colors_theme_count(void);
const char *colors_theme_name(int idx);

#endif // COLORS_H

/* ------------------------------------------------------------------------- */

#ifdef COLORS_IMPLEMENTATION

#include <stddef.h>

// Active palette (copied from the selected theme on switch).
ColorEntry g_palette[COLOR_COUNT];

static const ColorTheme COLORS_THEMES[] = {
    // Default theme - base16 inspired
    {
        .name = "default",
        .palette = {
            // Base colors (0-7: dark to light)
            COLOR_RGB(0x20, 0x27, 0x46),   // BASE0 - darkest background
            COLOR_RGB(0x29, 0x32, 0x56),   // BASE1
            COLOR_RGB(0x5e, 0x66, 0x87),   // BASE2
            COLOR_RGB(0x6b, 0x73, 0x94),   // BASE3
            COLOR_RGB(0x89, 0x8e, 0xa4),   // BASE4
            COLOR_RGB(0x97, 0x9d, 0xb4),   // BASE5
            COLOR_RGB(0xdf, 0xe2, 0xf1),   // BASE6 - bright foreground
            COLOR_RGB(0xf5, 0xf7, 0xff),   // BASE7 - brightest

            // Accent colors (8-15)
            COLOR_RGB(0xc9, 0x49, 0x22),   // BASE8 - red
            COLOR_RGB(0xc7, 0x6b, 0x29),   // BASE9 - orange
            COLOR_RGB(0xc0, 0x8b, 0x30),   // BASE10 - yellow
            COLOR_RGB(0xac, 0x97, 0x39),   // BASE11 - green
            COLOR_RGB(0x22, 0xa2, 0xc9),   // BASE12 - light blue
            COLOR_RGB(0x3d, 0x8f, 0xd1),   // BASE13 - blue
            COLOR_RGB(0x66, 0x79, 0xcc),   // BASE14 - violet
            COLOR_RGB(0x9c, 0x63, 0x7a),   // BASE15 - magenta

            // UI colors (references to base colors)
            COLOR_REF(COLOR_BASE5),        // UI_MSG_SYSTEM
            COLOR_REF(COLOR_BASE10),       // UI_MSG_PROMPT (yellow)
            COLOR_REF(COLOR_BASE12),       // UI_MSG_ITEM (light blue)
            COLOR_REF(COLOR_BASE7),        // UI_MSG_USER (brightest)
            COLOR_REF(COLOR_BASE8),        // UI_MSG_ERROR (red)
            COLOR_REF(COLOR_BASE6),        // UI_MSG_DEFAULT
            COLOR_REF(COLOR_BASE14),       // UI_MSG_DESC
            COLOR_REF(COLOR_BASE7),        // UI_PROMPT (bright white)
            COLOR_REF(COLOR_BASE7),        // UI_CURSOR_FG
            COLOR_REF(COLOR_BASE2),        // UI_CURSOR_BG
            COLOR_REF(COLOR_BASE3),        // UI_DIM
            COLOR_REF(COLOR_BASE6),        // UI_TYPED
            COLOR_REF(COLOR_BASE1),        // UI_BORDER
            COLOR_REF(COLOR_BASE2),        // UI_BORDER_FLOAT

            // Effects
            COLOR_RGB(181, 137, 0),        // TORCH
            COLOR_RGB(138, 3, 3),          // BLOOD
            COLOR_REF(COLOR_BASE8),        // DEAD
            COLOR_RGB(200, 50, 255),       // NEON_PURPLE
            COLOR_RGB(0, 0, 0),            // NONE
            COLOR_RGB(237, 240, 86),       // MUZZLE_FLASH
            COLOR_RGB(255, 0, 255),        // DEBUG
        }
    },

    // High contrast theme
    {
        .name = "high-contrast",
        .palette = {
            // Base colors (stronger contrast)
            COLOR_RGB(0x1a, 0x1a, 0x1a),   // BASE0 - near black
            COLOR_RGB(0x4a, 0x4a, 0x4a),   // BASE1
            COLOR_RGB(0x6a, 0x6a, 0x6a),   // BASE2
            COLOR_RGB(0x8a, 0x8a, 0x8a),   // BASE3
            COLOR_RGB(0xaa, 0xaa, 0xaa),   // BASE4
            COLOR_RGB(0xca, 0xca, 0xca),   // BASE5
            COLOR_RGB(0xf0, 0xf0, 0xf0),   // BASE6
            COLOR_RGB(0xff, 0xff, 0xff),   // BASE7 - pure white

            // Accent colors (more saturated)
            COLOR_RGB(0xff, 0x00, 0x00),   // BASE8 - red
            COLOR_RGB(0xff, 0x80, 0x00),   // BASE9 - orange
            COLOR_RGB(0xff, 0xff, 0x00),   // BASE10 - yellow
            COLOR_RGB(0x00, 0xff, 0x00),   // BASE11 - green
            COLOR_RGB(0x00, 0xff, 0xff),   // BASE12 - cyan
            COLOR_RGB(0x00, 0x80, 0xff),   // BASE13 - blue
            COLOR_RGB(0x80, 0x80, 0xff),   // BASE14 - violet
            COLOR_RGB(0xff, 0x00, 0xff),   // BASE15 - magenta

            // UI colors (references)
            COLOR_REF(COLOR_BASE5),
            COLOR_REF(COLOR_BASE10),
            COLOR_REF(COLOR_BASE12),
            COLOR_REF(COLOR_BASE7),
            COLOR_REF(COLOR_BASE8),
            COLOR_REF(COLOR_BASE6),
            COLOR_REF(COLOR_BASE14),       // UI_MSG_DESC
            COLOR_REF(COLOR_BASE4),
            COLOR_REF(COLOR_BASE7),
            COLOR_REF(COLOR_BASE2),
            COLOR_REF(COLOR_BASE3),
            COLOR_REF(COLOR_BASE6),
            COLOR_REF(COLOR_BASE1),
            COLOR_REF(COLOR_BASE2),

            // Effects
            COLOR_RGB(255, 200, 0),
            COLOR_RGB(200, 0, 0),
            COLOR_RGB(255, 0, 100),
            COLOR_RGB(200, 50, 255),
            COLOR_RGB(0, 0, 0),
            COLOR_RGB(255, 255, 100),
            COLOR_RGB(255, 0, 255),
        }
    },

    // Solarized dark theme
    {
        .name = "solarized",
        .palette = {
            // Base colors (solarized dark)
            COLOR_RGB(0x00, 0x2b, 0x36),   // BASE0 - base03
            COLOR_RGB(0x07, 0x36, 0x42),   // BASE1 - base02
            COLOR_RGB(0x58, 0x6e, 0x75),   // BASE2 - base01
            COLOR_RGB(0x65, 0x7b, 0x83),   // BASE3 - base00
            COLOR_RGB(0x83, 0x94, 0x96),   // BASE4 - base0
            COLOR_RGB(0x93, 0xa1, 0xa1),   // BASE5 - base1
            COLOR_RGB(0xee, 0xe8, 0xd5),   // BASE6 - base2
            COLOR_RGB(0xfd, 0xf6, 0xe3),   // BASE7 - base3

            // Accent colors (solarized accents)
            COLOR_RGB(0xdc, 0x32, 0x2f),   // BASE8 - red
            COLOR_RGB(0xcb, 0x4b, 0x16),   // BASE9 - orange
            COLOR_RGB(0xb5, 0x89, 0x00),   // BASE10 - yellow
            COLOR_RGB(0x85, 0x99, 0x00),   // BASE11 - green
            COLOR_RGB(0x2a, 0xa1, 0x98),   // BASE12 - cyan
            COLOR_RGB(0x26, 0x8b, 0xd2),   // BASE13 - blue
            COLOR_RGB(0x6c, 0x71, 0xc4),   // BASE14 - violet
            COLOR_RGB(0xd3, 0x36, 0x82),   // BASE15 - magenta

            // UI colors (references)
            COLOR_REF(COLOR_BASE5),
            COLOR_REF(COLOR_BASE10),
            COLOR_REF(COLOR_BASE12),
            COLOR_REF(COLOR_BASE7),
            COLOR_REF(COLOR_BASE8),
            COLOR_REF(COLOR_BASE6),
            COLOR_REF(COLOR_BASE14),       // UI_MSG_DESC
            COLOR_REF(COLOR_BASE4),
            COLOR_REF(COLOR_BASE7),
            COLOR_REF(COLOR_BASE2),
            COLOR_REF(COLOR_BASE3),
            COLOR_REF(COLOR_BASE6),
            COLOR_REF(COLOR_BASE1),
            COLOR_REF(COLOR_BASE2),

            // Effects
            COLOR_RGB(0xb5, 0x89, 0x00),
            COLOR_RGB(0xdc, 0x32, 0x2f),
            COLOR_RGB(0xd3, 0x36, 0x82),
            COLOR_RGB(0x6c, 0x71, 0xc4),
            COLOR_RGB(0, 0, 0),
            COLOR_RGB(0xb5, 0x89, 0x00),
            COLOR_RGB(0xd3, 0x36, 0x82),
        }
    },

    // Solarized light theme
    {
        .name = "solarized-light",
        .palette = {
            // Base colors (solarized light - inverted from dark)
            COLOR_RGB(0xfd, 0xf6, 0xe3),   // BASE0 - base3 (lightest bg)
            COLOR_RGB(0xee, 0xe8, 0xd5),   // BASE1 - base2
            COLOR_RGB(0x93, 0xa1, 0xa1),   // BASE2 - base1
            COLOR_RGB(0x83, 0x94, 0x96),   // BASE3 - base0
            COLOR_RGB(0x65, 0x7b, 0x83),   // BASE4 - base00
            COLOR_RGB(0x58, 0x6e, 0x75),   // BASE5 - base01
            COLOR_RGB(0x07, 0x36, 0x42),   // BASE6 - base02 (dark fg)
            COLOR_RGB(0x00, 0x2b, 0x36),   // BASE7 - base03 (darkest)

            // Accent colors (same as solarized dark)
            COLOR_RGB(0xdc, 0x32, 0x2f),   // BASE8 - red
            COLOR_RGB(0xcb, 0x4b, 0x16),   // BASE9 - orange
            COLOR_RGB(0xb5, 0x89, 0x00),   // BASE10 - yellow
            COLOR_RGB(0x85, 0x99, 0x00),   // BASE11 - green
            COLOR_RGB(0x2a, 0xa1, 0x98),   // BASE12 - cyan
            COLOR_RGB(0x26, 0x8b, 0xd2),   // BASE13 - blue
            COLOR_RGB(0x6c, 0x71, 0xc4),   // BASE14 - violet
            COLOR_RGB(0xd3, 0x36, 0x82),   // BASE15 - magenta

            // UI colors (references)
            COLOR_REF(COLOR_BASE5),
            COLOR_REF(COLOR_BASE10),
            COLOR_REF(COLOR_BASE12),
            COLOR_REF(COLOR_BASE7),
            COLOR_REF(COLOR_BASE8),
            COLOR_REF(COLOR_BASE6),
            COLOR_REF(COLOR_BASE14),       // UI_MSG_DESC
            COLOR_REF(COLOR_BASE4),
            COLOR_REF(COLOR_BASE7),
            COLOR_REF(COLOR_BASE2),
            COLOR_REF(COLOR_BASE3),
            COLOR_REF(COLOR_BASE6),
            COLOR_REF(COLOR_BASE1),
            COLOR_REF(COLOR_BASE2),

            // Effects
            COLOR_RGB(0xb5, 0x89, 0x00),
            COLOR_RGB(0xdc, 0x32, 0x2f),
            COLOR_RGB(0xd3, 0x36, 0x82),
            COLOR_RGB(0x6c, 0x71, 0xc4),
            COLOR_RGB(0xfd, 0xf6, 0xe3),
            COLOR_RGB(0xb5, 0x89, 0x00),
            COLOR_RGB(0xd3, 0x36, 0x82),
        }
    },

    // Gruvbox dark theme
    {
        .name = "gruvbox",
        .palette = {
            // Base colors (gruvbox dark)
            COLOR_RGB(0x28, 0x28, 0x28),   // BASE0 - bg
            COLOR_RGB(0x3c, 0x38, 0x36),   // BASE1 - bg1
            COLOR_RGB(0x50, 0x49, 0x45),   // BASE2 - bg2
            COLOR_RGB(0x66, 0x5c, 0x54),   // BASE3 - bg3
            COLOR_RGB(0x7c, 0x6f, 0x64),   // BASE4 - bg4
            COLOR_RGB(0xa8, 0x99, 0x84),   // BASE5 - fg4
            COLOR_RGB(0xeb, 0xdb, 0xb2),   // BASE6 - fg
            COLOR_RGB(0xfb, 0xf1, 0xc7),   // BASE7 - fg0

            // Accent colors (gruvbox bright)
            COLOR_RGB(0xfb, 0x49, 0x34),   // BASE8 - red
            COLOR_RGB(0xfe, 0x80, 0x19),   // BASE9 - orange
            COLOR_RGB(0xfa, 0xbd, 0x2f),   // BASE10 - yellow
            COLOR_RGB(0xb8, 0xbb, 0x26),   // BASE11 - green
            COLOR_RGB(0x8e, 0xc0, 0x7c),   // BASE12 - aqua
            COLOR_RGB(0x83, 0xa5, 0x98),   // BASE13 - blue
            COLOR_RGB(0xd3, 0x86, 0x9b),   // BASE14 - purple
            COLOR_RGB(0xd3, 0x86, 0x9b),   // BASE15 - magenta (purple)

            // UI colors (references)
            COLOR_REF(COLOR_BASE5),
            COLOR_REF(COLOR_BASE10),
            COLOR_REF(COLOR_BASE12),
            COLOR_REF(COLOR_BASE7),
            COLOR_REF(COLOR_BASE8),
            COLOR_REF(COLOR_BASE6),
            COLOR_REF(COLOR_BASE14),       // UI_MSG_DESC
            COLOR_REF(COLOR_BASE4),
            COLOR_REF(COLOR_BASE7),
            COLOR_REF(COLOR_BASE2),
            COLOR_REF(COLOR_BASE3),
            COLOR_REF(COLOR_BASE6),
            COLOR_REF(COLOR_BASE1),
            COLOR_REF(COLOR_BASE2),

            // Effects
            COLOR_RGB(0xfa, 0xbd, 0x2f),   // TORCH (yellow)
            COLOR_RGB(0xfb, 0x49, 0x34),   // BLOOD (red)
            COLOR_RGB(0xd3, 0x86, 0x9b),   // DEAD (purple)
            COLOR_RGB(0xd3, 0x86, 0x9b),   // NEON_PURPLE
            COLOR_RGB(0x28, 0x28, 0x28),   // NONE (bg)
            COLOR_RGB(0xfa, 0xbd, 0x2f),   // MUZZLE_FLASH (yellow)
            COLOR_RGB(0xd3, 0x86, 0x9b),   // DEBUG (purple)
        }
    },

    // Molokai theme
    {
        .name = "molokai",
        .palette = {
            // Base colors (molokai dark)
            COLOR_RGB(0x1b, 0x1d, 0x1e),   // BASE0 - bg
            COLOR_RGB(0x23, 0x25, 0x26),   // BASE1
            COLOR_RGB(0x29, 0x37, 0x39),   // BASE2
            COLOR_RGB(0x3b, 0x3a, 0x32),   // BASE3
            COLOR_RGB(0x75, 0x71, 0x5e),   // BASE4 - comment
            COLOR_RGB(0x8f, 0x8f, 0x8f),   // BASE5
            COLOR_RGB(0xf8, 0xf8, 0xf2),   // BASE6 - fg
            COLOR_RGB(0xf8, 0xf8, 0xf0),   // BASE7 - bright fg

            // Accent colors (molokai)
            COLOR_RGB(0xf9, 0x26, 0x72),   // BASE8 - red/pink
            COLOR_RGB(0xfd, 0x97, 0x1f),   // BASE9 - orange
            COLOR_RGB(0xe6, 0xdb, 0x74),   // BASE10 - yellow
            COLOR_RGB(0xa6, 0xe2, 0x2e),   // BASE11 - green
            COLOR_RGB(0x66, 0xd9, 0xef),   // BASE12 - cyan
            COLOR_RGB(0x66, 0xd9, 0xef),   // BASE13 - blue (cyan)
            COLOR_RGB(0xae, 0x81, 0xff),   // BASE14 - purple
            COLOR_RGB(0xf9, 0x26, 0x72),   // BASE15 - magenta (pink)

            // UI colors (references)
            COLOR_REF(COLOR_BASE5),
            COLOR_REF(COLOR_BASE10),
            COLOR_REF(COLOR_BASE12),
            COLOR_REF(COLOR_BASE7),
            COLOR_REF(COLOR_BASE8),
            COLOR_REF(COLOR_BASE6),
            COLOR_REF(COLOR_BASE14),       // UI_MSG_DESC
            COLOR_REF(COLOR_BASE4),
            COLOR_REF(COLOR_BASE7),
            COLOR_REF(COLOR_BASE2),
            COLOR_REF(COLOR_BASE3),
            COLOR_REF(COLOR_BASE6),
            COLOR_REF(COLOR_BASE1),
            COLOR_REF(COLOR_BASE2),

            // Effects
            COLOR_RGB(0xfd, 0x97, 0x1f),   // TORCH (orange)
            COLOR_RGB(0xf9, 0x26, 0x72),   // BLOOD (pink)
            COLOR_RGB(0xf9, 0x26, 0x72),   // DEAD (pink)
            COLOR_RGB(0xae, 0x81, 0xff),   // NEON_PURPLE
            COLOR_RGB(0x1b, 0x1d, 0x1e),   // NONE (bg)
            COLOR_RGB(0xe6, 0xdb, 0x74),   // MUZZLE_FLASH (yellow)
            COLOR_RGB(0xae, 0x81, 0xff),   // DEBUG (purple)
        }
    },

    // Tokusa - monochrome green-grey theme
    {
        .name = "tokusa",
        .palette = {
            // Base colors (muted green-grey)
            COLOR_RGB(0x1a, 0x1c, 0x1a),   // BASE0 - darkest bg
            COLOR_RGB(0x24, 0x28, 0x24),   // BASE1
            COLOR_RGB(0x3a, 0x40, 0x3a),   // BASE2
            COLOR_RGB(0x4a, 0x52, 0x4a),   // BASE3
            COLOR_RGB(0x62, 0x6b, 0x62),   // BASE4
            COLOR_RGB(0x7a, 0x85, 0x7a),   // BASE5
            COLOR_RGB(0xb8, 0xc4, 0xb8),   // BASE6 - fg
            COLOR_RGB(0xd4, 0xde, 0xd4),   // BASE7 - bright fg

            // Accent colors (muted, desaturated)
            COLOR_RGB(0x9a, 0x5a, 0x5a),   // BASE8 - muted red
            COLOR_RGB(0x9a, 0x7a, 0x5a),   // BASE9 - muted orange
            COLOR_RGB(0x9a, 0x9a, 0x6a),   // BASE10 - muted yellow
            COLOR_RGB(0x6a, 0x8a, 0x6a),   // BASE11 - muted green
            COLOR_RGB(0x6a, 0x8a, 0x8a),   // BASE12 - muted cyan
            COLOR_RGB(0x6a, 0x7a, 0x8a),   // BASE13 - muted blue
            COLOR_RGB(0x7a, 0x6a, 0x8a),   // BASE14 - muted violet
            COLOR_RGB(0x8a, 0x6a, 0x7a),   // BASE15 - muted magenta

            // UI colors (references)
            COLOR_REF(COLOR_BASE5),
            COLOR_REF(COLOR_BASE10),
            COLOR_REF(COLOR_BASE12),
            COLOR_REF(COLOR_BASE7),
            COLOR_REF(COLOR_BASE8),
            COLOR_REF(COLOR_BASE6),
            COLOR_REF(COLOR_BASE14),       // UI_MSG_DESC
            COLOR_REF(COLOR_BASE4),
            COLOR_REF(COLOR_BASE7),
            COLOR_REF(COLOR_BASE2),
            COLOR_REF(COLOR_BASE3),
            COLOR_REF(COLOR_BASE6),
            COLOR_REF(COLOR_BASE1),
            COLOR_REF(COLOR_BASE2),

            // Effects
            COLOR_RGB(0x9a, 0x8a, 0x5a),   // TORCH (muted amber)
            COLOR_RGB(0x8a, 0x4a, 0x4a),   // BLOOD (muted red)
            COLOR_RGB(0x7a, 0x5a, 0x6a),   // DEAD (muted purple)
            COLOR_RGB(0x7a, 0x6a, 0x8a),   // NEON_PURPLE
            COLOR_RGB(0x1a, 0x1c, 0x1a),   // NONE (bg)
            COLOR_RGB(0xaa, 0x9a, 0x6a),   // MUZZLE_FLASH
            COLOR_RGB(0x7a, 0x6a, 0x8a),   // DEBUG
        }
    },

    // Koda - warm earthy brown theme
    {
        .name = "koda",
        .palette = {
            // Base colors (warm browns)
            COLOR_RGB(0x1c, 0x18, 0x14),   // BASE0 - dark brown bg
            COLOR_RGB(0x2a, 0x24, 0x1e),   // BASE1
            COLOR_RGB(0x3e, 0x36, 0x2e),   // BASE2
            COLOR_RGB(0x52, 0x48, 0x3e),   // BASE3
            COLOR_RGB(0x6e, 0x62, 0x56),   // BASE4
            COLOR_RGB(0x8a, 0x7e, 0x72),   // BASE5
            COLOR_RGB(0xc4, 0xb8, 0xaa),   // BASE6 - warm fg
            COLOR_RGB(0xe0, 0xd6, 0xc8),   // BASE7 - bright fg

            // Accent colors (earthy, warm)
            COLOR_RGB(0xa6, 0x5a, 0x4a),   // BASE8 - terracotta
            COLOR_RGB(0xb8, 0x7a, 0x4a),   // BASE9 - rust orange
            COLOR_RGB(0xc4, 0xa0, 0x5a),   // BASE10 - ochre
            COLOR_RGB(0x7a, 0x8a, 0x5a),   // BASE11 - olive
            COLOR_RGB(0x6a, 0x8a, 0x7a),   // BASE12 - sage
            COLOR_RGB(0x6a, 0x7a, 0x8a),   // BASE13 - slate blue
            COLOR_RGB(0x7a, 0x6a, 0x7e),   // BASE14 - dusty violet
            COLOR_RGB(0x8a, 0x6a, 0x6e),   // BASE15 - mauve

            // UI colors (references)
            COLOR_REF(COLOR_BASE5),
            COLOR_REF(COLOR_BASE10),
            COLOR_REF(COLOR_BASE12),
            COLOR_REF(COLOR_BASE7),
            COLOR_REF(COLOR_BASE8),
            COLOR_REF(COLOR_BASE6),
            COLOR_REF(COLOR_BASE14),       // UI_MSG_DESC
            COLOR_REF(COLOR_BASE4),
            COLOR_REF(COLOR_BASE7),
            COLOR_REF(COLOR_BASE2),
            COLOR_REF(COLOR_BASE3),
            COLOR_REF(COLOR_BASE6),
            COLOR_REF(COLOR_BASE1),
            COLOR_REF(COLOR_BASE2),

            // Effects
            COLOR_RGB(0xc4, 0x90, 0x50),   // TORCH (warm amber)
            COLOR_RGB(0x8a, 0x3a, 0x3a),   // BLOOD (dark red)
            COLOR_RGB(0x7a, 0x5a, 0x5a),   // DEAD (dusty rose)
            COLOR_RGB(0x7a, 0x6a, 0x7e),   // NEON_PURPLE
            COLOR_RGB(0x1c, 0x18, 0x14),   // NONE (bg)
            COLOR_RGB(0xd4, 0xa0, 0x60),   // MUZZLE_FLASH
            COLOR_RGB(0x7a, 0x6a, 0x7e),   // DEBUG
        }
    },
};

static int g_current_theme = 0;

// Cached resolution of the (very hot) background slot.
static Color colors_cached_base0;

// Resolve an entry to its final RGB value, following references. `depth` guards
// against circular references in a malformed theme.
static Color colors_resolve(const ColorEntry *entry, int depth) {
    if (depth > 8) {
        return (Color){ 0, 0, 0 };
    }
    if (!entry->is_ref) {
        return entry->color;
    }
    if (entry->ref < 0 || entry->ref >= COLOR_COUNT) {
        return (Color){ 0, 0, 0 };
    }
    return colors_resolve(&g_palette[entry->ref], depth + 1);
}

void colors_init(void) {
    colors_set_theme(0);
}

Color color_get(ColorIndex idx) {
    if (idx == COLOR_BASE0) return colors_cached_base0;
    if (idx < 0 || idx >= COLOR_COUNT) {
        return colors_resolve(&g_palette[COLOR_BASE2], 0);
    }
    return colors_resolve(&g_palette[idx], 0);
}

void colors_set_theme(int theme_idx) {
    int count = colors_theme_count();
    if (theme_idx < 0 || theme_idx >= count) {
        theme_idx = 0;
    }

    g_current_theme = theme_idx;

    // Copy the theme's palette into the active (mutable) palette.
    for (int i = 0; i < COLOR_COUNT; i++) {
        g_palette[i] = COLORS_THEMES[theme_idx].palette[i];
    }

    colors_cached_base0 = colors_resolve(&g_palette[COLOR_BASE0], 0);
}

int colors_get_theme(void) {
    return g_current_theme;
}

int colors_theme_count(void) {
    return (int)(sizeof(COLORS_THEMES) / sizeof(COLORS_THEMES[0]));
}

const char *colors_theme_name(int idx) {
    if (idx < 0 || idx >= colors_theme_count()) {
        return NULL;
    }
    return COLORS_THEMES[idx].name;
}

#endif // COLORS_IMPLEMENTATION
