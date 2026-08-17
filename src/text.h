
#ifndef TEXT_H
#define TEXT_H

#include <stddef.h>

void text_one_line(const char *in, char *out, size_t size);

void text_chomp(char *s);

char *text_slurp(const char *path, size_t max_bytes, size_t *len_out);

double now_seconds(void);

int path_config_dir(char *out, size_t size);

int path_config_file(char *out, size_t size, const char *leaf);

void path_home_relative(const char *dir, char *out, size_t size);

char *path_expand_home(const char *path);

#endif
