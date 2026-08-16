
#ifndef BASH_H
#define BASH_H

int bash_is_command(const char *line);

void bash_run(const char *line);

/* The last run's command and output, phrased for the agent. Caller frees. */
char *bash_take_context(void);

#endif
