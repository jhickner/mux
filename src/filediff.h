
#ifndef FILEDIFF_H
#define FILEDIFF_H

void filediff_snapshot(const char *path);

int filediff_render(void);

int filediff_render_patch(const char *patch);

void filediff_clear(void);

#endif
