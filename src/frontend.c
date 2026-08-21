#include "frontend.h"

#define FRONTEND_MAX 8

static unsigned stack[FRONTEND_MAX];
static int      depth;

void frontend_push(unsigned caps)
{
    // Counted past the limit, so push and pop pair up and an overflow cannot
    // pop somebody else's caps.
    if (depth < FRONTEND_MAX)
        stack[depth] = caps;
    depth++;
}

void frontend_pop(void)
{
    if (depth > 0)
        depth--;
}

unsigned frontend_caps(void)
{
    if (depth == 0)
        return FRONTEND_KEYBOARD;
    // Nested past the limit: the innermost known caps still stand.
    int at = depth < FRONTEND_MAX ? depth : FRONTEND_MAX;
    return stack[at - 1];
}

int frontend_has_keyboard(void)
{
    return (frontend_caps() & FRONTEND_KEYBOARD) != 0;
}
