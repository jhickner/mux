
#ifndef IMAGE_H
#define IMAGE_H

#include "vendor/cJSON.h"

#define IMAGE_ROWS_DEFAULT 20
#define IMAGE_ROWS_MIN     2
#define IMAGE_ROWS_MAX     100

void image_init(void);

void image_set_rows(int rows);
int  image_rows(void);

int image_available(void);

int image_show(const char *path, int indent);

// The cell box an image of img_w x img_h pixels is drawn into, given the cell
// size and the room available. Never larger than the image's own size.
void image_fit(int img_w, int img_h, int cw, int ch, int cols_box, int rows_box,
               int *cols, int *rows);

void image_poll(void);

void image_wait(void);

// A placement, carried across a restart.
#define IMAGE_PLACED_KIND "image"
void image_placed_load(const cJSON *st);

#endif
