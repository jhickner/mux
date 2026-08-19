
#ifndef FILEDIFF_H
#define FILEDIFF_H

void filediff_snapshot(const char *path);

// What changed since the snapshot, as patch text, and the snapshot is spent.
// The caller keeps it and hands it back to filediff_render_patch whenever the
// diff has to be drawn again. NULL when nothing changed.
char *filediff_take_patch(void);

// Whether a patch has any changed line to show, for a caller deciding between
// the diff and something else.
int filediff_patch_draws(const char *patch);

int filediff_render_patch(const char *patch);

void filediff_clear(void);

#endif
