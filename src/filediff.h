
#ifndef FILEDIFF_H
#define FILEDIFF_H

void filediff_snapshot(const char *path);

// What changed since the snapshot, as patch text; spends the snapshot. NULL
// when nothing changed. Hand it back to render_patch to draw it again.
char *filediff_take_patch(void);

int filediff_patch_draws(const char *patch);

int filediff_render_patch(const char *patch);

void filediff_clear(void);

#endif
