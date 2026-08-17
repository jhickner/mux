
#ifndef IMAGE_H
#define IMAGE_H

#define IMAGE_ROWS_DEFAULT 20
#define IMAGE_ROWS_MIN     2
#define IMAGE_ROWS_MAX     100

void image_init(void);

void image_set_rows(int rows);
int  image_rows(void);

int image_available(void);

int image_show(const char *path, int indent);

void image_poll(void);

void image_wait(void);

#endif
