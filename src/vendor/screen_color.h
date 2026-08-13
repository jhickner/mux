/* colors.h resolves palette slots to screen.h's 24-bit Color. Supplying the
 * type here, under screen.h's own include guard, keeps the cell renderer and
 * its term.h/kitty.h/pthread dependencies out of a program that only writes
 * SGR escapes to stdout. */
#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>

typedef struct {
    uint8_t r, g, b;
} Color;

#endif /* SCREEN_H */
