#include "transcript.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

static const char INTRO[] =
    "You are continuing a conversation that began in another coding-agent "
    "backend. The transcript below is prior dialogue, not a new request. "
    "Use it to preserve the user's goals, decisions, and unfinished work. "
    "Do not answer the transcript itself; wait for and answer the next user "
    "message. The current workspace is shared with the prior backend and its "
    "files are authoritative.\n\n";

static void turn_free(struct transcript_turn *turn)
{
    free(turn->backend);
    free(turn->user);
    free(turn->assistant);
    memset(turn, 0, sizeof *turn);
}

void transcript_clear(struct transcript *t)
{
    if (!t)
        return;
    for (size_t i = 0; i < t->count; i++)
        turn_free(&t->turns[i]);
    t->count = 0;
}

void transcript_free(struct transcript *t)
{
    if (!t)
        return;
    transcript_clear(t);
    free(t->turns);
    memset(t, 0, sizeof *t);
}

int transcript_add(struct transcript *t, const char *backend, const char *user,
                   const char *assistant, int interrupted)
{
    if (!t || !user || !assistant)
        return 0;
    if (t->count == t->capacity) {
        size_t capacity = t->capacity ? t->capacity * 2 : 8;
        struct transcript_turn *grown = realloc(t->turns, capacity * sizeof *grown);
        if (!grown)
            return 0;
        t->turns = grown;
        t->capacity = capacity;
    }

    struct transcript_turn turn = {
        .backend = strdup(backend && *backend ? backend : "unknown"),
        .user = strdup(user),
        .assistant = strdup(assistant),
        .interrupted = interrupted,
    };
    if (!turn.backend || !turn.user || !turn.assistant) {
        turn_free(&turn);
        return 0;
    }
    t->turns[t->count++] = turn;
    return 1;
}

static size_t turn_size(const struct transcript_turn *turn)
{

    return strlen(turn->backend) + strlen(turn->user) + strlen(turn->assistant) + 160;
}

static int append_text(char **out, size_t *length, size_t *capacity, const char *text)
{
    size_t add = strlen(text);
    if (*length + add + 1 > *capacity) {
        size_t cap = *capacity ? *capacity : 1024;
        while (cap < *length + add + 1)
            cap *= 2;
        char *grown = realloc(*out, cap);
        if (!grown)
            return 0;
        *out = grown;
        *capacity = cap;
    }
    memcpy(*out + *length, text, add + 1);
    *length += add;
    return 1;
}

char *transcript_handoff(const struct transcript *t, size_t max_bytes)
{
    if (!t || t->count == 0)
        return NULL;
    if (max_bytes < sizeof INTRO + 256)
        max_bytes = sizeof INTRO + 256;

    size_t first = t->count;
    size_t used = sizeof INTRO;
    for (size_t i = t->count; i > 0; i--) {
        size_t need = turn_size(&t->turns[i - 1]);
        if (used + need > max_bytes && first < t->count)
            break;
        first = i - 1;
        used += need;
    }

    char *out = NULL;
    size_t length = 0, capacity = 0;
    if (!append_text(&out, &length, &capacity, INTRO))
        return NULL;
    if (first > 0) {
        char omitted[128];
        snprintf(omitted, sizeof omitted,
                 "[" APP_NAME " omitted %zu older turn%s to fit the handoff.]\n\n",
                 first, first == 1 ? "" : "s");
        if (!append_text(&out, &length, &capacity, omitted)) {
            free(out);
            return NULL;
        }
    }

    for (size_t i = first; i < t->count; i++) {
        const struct transcript_turn *turn = &t->turns[i];
        char heading[256];
        snprintf(heading, sizeof heading, "--- turn %zu (%s%s) ---\nUSER [%zu bytes]\n",
                 i + 1, turn->backend, turn->interrupted ? ", interrupted" : "",
                 strlen(turn->user));
        if (!append_text(&out, &length, &capacity, heading) ||
            !append_text(&out, &length, &capacity, turn->user) ||
            !append_text(&out, &length, &capacity, "\nASSISTANT [")) {
            free(out);
            return NULL;
        }
        char count[64];
        snprintf(count, sizeof count, "%zu bytes]\n", strlen(turn->assistant));
        if (!append_text(&out, &length, &capacity, count) ||
            !append_text(&out, &length, &capacity, turn->assistant) ||
            !append_text(&out, &length, &capacity, "\n\n")) {
            free(out);
            return NULL;
        }
    }
    return out;
}
