
#ifndef BASH_H
#define BASH_H

int bash_is_command(const char *line);

const char *bash_body(const char *line);

void bash_run(const char *line);

#endif
