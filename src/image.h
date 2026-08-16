
#ifndef IMAGE_H
#define IMAGE_H

#define IMG_ROWS_DEFAULT 20
#define IMG_ROWS_MIN     2
#define IMG_ROWS_MAX     100

void img_init(void);

void img_set_rows(int rows);
int  img_rows(void);

int img_available(void);

int img_show(const char *path, int indent);

#endif
