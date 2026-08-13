/* The single translation unit that instantiates the vendored single-header
 * libraries. Everything else includes their headers declaration-only. */

#define CLAUDE_IMPLEMENTATION
#include "claude.h"

#define REPL_IMPLEMENTATION
#include "repl.h"

#include "screen_color.h"
#define COLORS_IMPLEMENTATION
#include "colors.h"
