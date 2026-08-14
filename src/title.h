/* title.h — the short name a conversation earns from its first turn.
 *
 * Claude Code writes its own into the transcript as an "ai-title" record; ours
 * live in ~/.config/mux/titles as "id<TAB>title" lines, so a title
 * survives the process that asked for it and shows up in /resume. */
#ifndef TITLE_H
#define TITLE_H

#include <stddef.h>

/* Copy the cached title for `id` into `out`. Returns 1 when one was found. */
int title_lookup(const char *id, char *out, size_t size);

/* Have the active backend name the conversation and cache it under `id`.
 * Claude uses a fixed small model; the other backends use `model`, or their
 * configured default when it is NULL. The detached helper is ephemeral, so
 * this returns at once and the title turns up on a later lookup. Asking again
 * for an id that already has a title replaces it, which is how a cleared
 * conversation earns a new name. */
void title_request(const char *id, const char *backend, const char *model,
                   const char *cwd, const char *prompt, const char *reply);

#endif /* TITLE_H */
