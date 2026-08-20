
#ifndef BASH_H
#define BASH_H

#include "vendor/cJSON.h"

int bash_is_command(const char *line);

void bash_run(const char *line);

char *bash_take_context(void);

// Carried across a restart.
#define BASH_RAN_KIND "bash"
void bash_ran_load(const cJSON *st);

#endif
