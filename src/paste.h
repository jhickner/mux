/* Ctrl-V: the image sitting on the system clipboard, saved to a file so the
 * agent can be pointed at it. Text pasting is the terminal's job, not ours. */
#ifndef PASTE_H
#define PASTE_H

#include <stddef.h>

/* Write the clipboard's image as a PNG under ~/.config/simple-agent/pastes and
 * put its path in `out`. Returns 0 when the clipboard holds no image, when
 * HOME is unset, or on a platform with no way to ask. */
int paste_image(char *out, size_t size);

#endif /* PASTE_H */
