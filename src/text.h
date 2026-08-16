
#ifndef TEXT_H
#define TEXT_H

#include <stddef.h>

/* Collapses every run of whitespace to a single space and trims both ends. */
void text_one_line(const char *in, char *out, size_t size);

/* Strips trailing newline and carriage-return bytes in place. */
void text_chomp(char *s);

/* Reads a whole regular file. Returns a NUL-terminated heap buffer the caller
   frees, or NULL. max_bytes of 0 means no limit; len_out may be NULL. */
char *text_slurp(const char *path, size_t max_bytes, size_t *len_out);

double now_seconds(void);

/* Builds ~/.config/<app>, creating it. Returns 0 without $HOME or on
   truncation. */
int path_config_dir(char *out, size_t size);

/* path_config_dir plus "/<leaf>". */
int path_config_file(char *out, size_t size, const char *leaf);

/* Rewrites a leading $HOME as "~". Tolerates a NULL dir. */
void path_home_relative(const char *dir, char *out, size_t size);

/* Expands a leading "~" to $HOME. Returns a heap buffer, or NULL when the path
   does not start with "~" (or $HOME is unset). */
char *path_expand_home(const char *path);

#endif
