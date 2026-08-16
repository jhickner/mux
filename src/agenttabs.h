
#ifndef AGENTTABS_H
#define AGENTTABS_H

void agenttabs_begin(const char *backend);

void agenttabs_working(void);
void agenttabs_finished(void);
void agenttabs_errored(void);

void agenttabs_usage(int percent, long resets_at, long window_minutes);

void agenttabs_forget_hook(const char *id);

#endif
