/* Ctrl-V: the system clipboard. An image is saved to a file so the agent can be
 * pointed at it; anything else comes back as text. */
#ifndef PASTE_H
#define PASTE_H

#include <stddef.h>

/* Write the clipboard's image as a PNG under ~/.config/mux/pastes and
 * put its path in `out`. Returns 0 when the clipboard holds no image, when
 * HOME is unset, or on a platform with no way to ask. */
int paste_image(char *out, size_t size);

/* The clipboard's text, malloc'd for the caller, or NULL when it holds none. */
char *paste_text(void);

#endif /* PASTE_H */
