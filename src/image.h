/* Inline images: a markdown image in a reply is drawn in the terminal with the
 * kitty graphics protocol, as unicode placeholder cells so it survives tmux. */
#ifndef IMAGE_H
#define IMAGE_H

/* Tall enough for a render to read, short enough that a reply with several
 * images still scrolls sensibly. */
#define IMG_ROWS_DEFAULT 20
#define IMG_ROWS_MIN     2
#define IMG_ROWS_MAX     100

/* Detect terminal support and tmux passthrough. Safe to call more than once. */
void img_init(void);

/* The tallest an image may be drawn, in terminal rows; the width follows from
 * the aspect ratio. Clamped to [IMG_ROWS_MIN, IMG_ROWS_MAX], and to what the
 * window can show. */
void img_set_rows(int rows);
int  img_rows(void);

/* Nonzero when img_show() can draw. */
int img_available(void);

/* Draw `path` at `indent`, occupying whole cells and leaving the cursor on the
 * row below. Returns 0 when the file could not be drawn, so the caller can fall
 * back to rendering the markdown as text. */
int img_show(const char *path, int indent);

#endif /* IMAGE_H */
