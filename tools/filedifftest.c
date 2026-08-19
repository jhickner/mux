#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "filediff.h"
#include "ui.h"

static int failures;

static void fail(const char *what, const char *out)
{
    fprintf(stderr, "FAIL %s\n---\n%s---\n", what, out ? out : "");
    failures++;
}

static char *render(const char *patch, int width, int *drew)
{
    ui_capture_begin(width);
    int d = filediff_render_patch(patch);
    char *out = ui_capture_end();
    if (drew)
        *drew = d;
    return out;
}

// A patch from the backend: file names, hunks, and the changed lines.
static void check_supplied(void)
{
    const char *patch =
        "@@file src/one.c\n"
        "@@ -1,2 +1,2 @@\n"
        " context\n"
        "-old\n"
        "+new\n"
        "@@file src/two.c\n"
        "@@ -3 +3 @@\n"
        "-before\n"
        "+after\n";

    int drew = 0;
    char *out = render(patch, 80, &drew);
    if (!drew || !out || !strstr(out, "src/one.c") || !strstr(out, "src/two.c") ||
        !strstr(out, "- old") || !strstr(out, "+ new") || !strstr(out, "- before") ||
        !strstr(out, "+ after"))
        fail("a supplied patch is rendered", out);
    free(out);
}

// A patch taken from a snapshot is text, so the same diff can be drawn again
// at another width — the file it came from may have changed since, and the
// rows it drew the first time belong to the width it drew them at.
static void check_snapshot(void)
{
    char path[] = "/tmp/mux-filedifftest-XXXXXX";
    int  fd = mkstemp(path);
    if (fd < 0) {
        fail("a temp file to diff", NULL);
        return;
    }
    const char *before = "keep one\nkeep two\nthe line that changes, which is long "
                         "enough to need cutting short somewhere\nkeep three\n";
    (void)!write(fd, before, strlen(before));
    close(fd);

    filediff_snapshot(path);

    FILE *f = fopen(path, "w");
    if (!f) {
        fail("rewriting the file under the snapshot", NULL);
        unlink(path);
        return;
    }
    fputs("keep one\nkeep two\nthe line that replaced it, which is also long "
          "enough to need cutting short\nkeep three\n", f);
    fclose(f);

    char *patch = filediff_take_patch();
    if (!filediff_patch_draws(patch)) {
        fail("a changed file yields a patch", patch);
        free(patch);
        unlink(path);
        return;
    }

    // The file goes away: nothing may be read from it again.
    unlink(path);

    char *wide = render(patch, 100, NULL);
    char *narrow = render(patch, 40, NULL);

    if (!wide || !strstr(wide, "- the line that changes") ||
        !strstr(wide, "+ the line that replaced it"))
        fail("the patch still has both sides of the change", wide);
    if (!narrow || !strstr(narrow, "\xe2\x80\xa6"))
        fail("a narrow pane cuts the diff short rather than overflowing it", narrow);
    if (wide && narrow && strlen(wide) == strlen(narrow))
        fail("the diff is laid out for the width it is drawn at", narrow);

    // Context is kept, but only around the change.
    if (!wide || !strstr(wide, "keep two") || !strstr(wide, "keep three"))
        fail("the lines around the change are kept", wide);

    free(wide);
    free(narrow);
    free(patch);
}

// Nothing changed: no patch, so the caller knows to show the output instead.
static void check_unchanged(void)
{
    char path[] = "/tmp/mux-filedifftest-XXXXXX";
    int  fd = mkstemp(path);
    if (fd < 0) {
        fail("a temp file to diff", NULL);
        return;
    }
    (void)!write(fd, "same\n", 5);
    close(fd);

    filediff_snapshot(path);
    char *patch = filediff_take_patch();
    if (patch)
        fail("an unchanged file yields no patch", patch);
    free(patch);
    unlink(path);
}

int main(void)
{
    ui_init();

    check_supplied();
    check_snapshot();
    check_unchanged();

    if (failures)
        return 1;
    puts("filedifftest: all checks passed");
    return 0;
}
