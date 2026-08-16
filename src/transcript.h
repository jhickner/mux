
#ifndef TRANSCRIPT_H
#define TRANSCRIPT_H

#include <stddef.h>

struct transcript_turn {
    char *backend;
    char *user;
    char *assistant;
    int   interrupted;
};

struct transcript {
    struct transcript_turn *turns;
    size_t count;
    size_t capacity;
};

void transcript_free(struct transcript *t);
void transcript_clear(struct transcript *t);
int  transcript_add(struct transcript *t, const char *backend, const char *user,
                    const char *assistant, int interrupted);

char *transcript_handoff(const struct transcript *t, size_t max_bytes);

#endif
