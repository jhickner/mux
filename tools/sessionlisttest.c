
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sessionlist.h"
#include "title.h"

/* Stub: the harness links sessionlist.o without title.o. */
int title_lookup(const char *id, char *out, size_t size)
{
    (void)id;
    (void)out;
    (void)size;
    return 0;
}

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

static int make_fixture(const char *group, int number)
{
    char id[32], dir[1024], summary[1100];
    snprintf(id, sizeof id, "session-%02d", number);
    snprintf(dir, sizeof dir, "%s/%s", group, id);
    if (mkdir(dir, 0700) != 0)
        return 0;

    snprintf(summary, sizeof summary, "%s/summary.json", dir);
    FILE *f = fopen(summary, "w");
    if (!f)
        return 0;
    fprintf(f, "{\"generated_title\":\"Session %02d\"}\n", number);
    if (fclose(f) != 0)
        return 0;

    struct timespec times[2] = {{1000 + number, 0}, {1000 + number, 0}};
    return utimensat(AT_FDCWD, dir, times, 0) == 0;
}

static void check_list(const char *skip_id, int newest, int oldest)
{
    struct past_session *list = NULL;
    int count = sessionlist_load("grok", "/project", skip_id, &list);
    if (count != 40) {
        fail("session count");
        free(list);
        return;
    }

    int expected = newest;
    for (int i = 0; i < count; i++) {
        char id[32];
        snprintf(id, sizeof id, "session-%02d", expected);
        if (strcmp(list[i].id, id) != 0) {
            fail("newest sessions in descending order");
            break;
        }
        do {
            expected--;
        } while (skip_id && expected >= oldest &&
                 snprintf(id, sizeof id, "session-%02d", expected) > 0 &&
                 strcmp(id, skip_id) == 0);
    }
    if (expected != oldest - 1)
        fail("oldest retained session");
    free(list);
}

int main(void)
{
    char root[] = "/tmp/mux-sessionlist-XXXXXX";
    if (!mkdtemp(root)) {
        fail("temporary directory");
        return 1;
    }
    setenv("GROK_HOME", root, 1);

    char sessions[1024], group[1100];
    snprintf(sessions, sizeof sessions, "%s/sessions", root);
    snprintf(group, sizeof group, "%s/%%2Fproject", sessions);
    if (mkdir(sessions, 0700) != 0 || mkdir(group, 0700) != 0) {
        fail("fixture directories");
        return 1;
    }

    for (int i = 0; i < 45; i++)
        if (!make_fixture(group, i)) {
            fail("session fixture");
            return 1;
        }

    check_list(NULL, 44, 5);
    check_list("session-44", 43, 4);

    for (int i = 0; i < 45; i++) {
        char dir[1200], summary[1300];
        snprintf(dir, sizeof dir, "%s/session-%02d", group, i);
        snprintf(summary, sizeof summary, "%s/summary.json", dir);
        unlink(summary);
        rmdir(dir);
    }
    rmdir(group);
    rmdir(sessions);
    rmdir(root);

    if (!failures)
        printf("sessionlisttest: all checks passed\n");
    return failures != 0;
}
