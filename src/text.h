
#ifndef TEXT_H
#define TEXT_H

#include <stddef.h>
#include <stdint.h>

void text_one_line(const char *in, char *out, size_t size);

void text_block(const char *in, char *out, size_t size);

void text_chomp(char *s);

char *text_slurp(const char *path, size_t max_bytes, size_t *len_out);

int text_shell_quote(const char *s, char *out, size_t size);

int text_fuzzy_score(const char *name, const char *q);

size_t text_utf8_encode(uint32_t cp, char out[4]);

double now_seconds(void);

int path_config_dir(char *out, size_t size);

int path_config_file(char *out, size_t size, const char *leaf);

void path_home_relative(const char *dir, char *out, size_t size);

char *path_expand_home(const char *path);

#endif
