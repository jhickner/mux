#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "filediff.h"
#include "ui.h"

int main(void)
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

    FILE *capture = tmpfile();
    int saved = dup(STDOUT_FILENO);
    if (!capture || saved < 0 || dup2(fileno(capture), STDOUT_FILENO) < 0)
        return 1;

    ui_init();
    int drew = filediff_render_patch(patch);
    fflush(stdout);
    long size = ftell(capture);
    rewind(capture);
    char *out = size >= 0 ? calloc((size_t)size + 1, 1) : NULL;
    if (out && size > 0)
        fread(out, 1, (size_t)size, capture);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    fclose(capture);

    int ok = drew && out && strstr(out, "src/one.c") && strstr(out, "src/two.c") &&
             strstr(out, "- old") && strstr(out, "+ new") &&
             strstr(out, "- before") && strstr(out, "+ after");
    free(out);
    if (!ok) {
        fprintf(stderr, "filedifftest: supplied patch was not rendered\n");
        return 1;
    }
    puts("filedifftest: all checks passed");
    return 0;
}
