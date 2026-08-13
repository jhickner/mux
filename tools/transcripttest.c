#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transcript.h"

static void require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "transcripttest: %s\n", message);
        exit(1);
    }
}

int main(void)
{
    struct transcript t = {0};
    require(transcript_handoff(&t, 4096) == NULL, "empty transcript made a handoff");
    require(transcript_add(&t, "claude", "first question", "first answer", 0),
            "could not add first turn");
    require(transcript_add(&t, "codex", "latest question", "latest answer", 1),
            "could not add latest turn");

    char *all = transcript_handoff(&t, 4096);
    require(all != NULL, "could not build handoff");
    require(strstr(all, "prior dialogue, not a new request") != NULL,
            "continuation instructions missing");
    require(strstr(all, "first question") != NULL && strstr(all, "first answer") != NULL,
            "first turn missing");
    require(strstr(all, "latest question") != NULL && strstr(all, "latest answer") != NULL,
            "latest turn missing");
    require(strstr(all, "codex, interrupted") != NULL, "turn metadata missing");
    free(all);

    char large[1200];
    memset(large, 'x', sizeof large - 1);
    large[sizeof large - 1] = '\0';
    transcript_clear(&t);
    require(transcript_add(&t, "claude", large, large, 0), "could not add old turn");
    require(transcript_add(&t, "claude", "keep this", "and this", 0),
            "could not add recent turn");
    char *bounded = transcript_handoff(&t, 900);
    require(bounded != NULL, "could not build bounded handoff");
    require(strstr(bounded, "omitted 1 older turn") != NULL, "omission not reported");
    require(strstr(bounded, "keep this") != NULL && strstr(bounded, "and this") != NULL,
            "recent turn was not retained");
    require(strstr(bounded, large) == NULL, "oversized old turn was retained");
    free(bounded);

    transcript_free(&t);
    require(t.turns == NULL && t.count == 0 && t.capacity == 0,
            "free did not reset transcript");
    puts("transcripttest: ok");
    return 0;
}
