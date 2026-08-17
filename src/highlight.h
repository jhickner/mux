
#ifndef HIGHLIGHT_H
#define HIGHLIGHT_H

#include <stddef.h>

/* Fills roles[0..len) with one enum ui_role per byte of a shell command.
 * Regions the command hands to another interpreter — python -c bodies, python
 * or shell heredocs — are highlighted with that language's rules instead. */
void highlight_shell(const char *text, size_t len, unsigned char *roles);

#endif
