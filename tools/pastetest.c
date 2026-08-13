/* Manual check for the Ctrl-V clipboard grab, which needs a real pasteboard
 * and so cannot join `make check`.
 *
 *   make pastetest
 *   ./pastetest                  # whatever is on the clipboard now
 *   ./pastetest some/image.png   # put that on the clipboard first, then grab
 *
 * Prints the path it wrote, or the reason nothing came back. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "paste.h"

static int put_on_clipboard(const char *file)
{
    if (strchr(file, '\'')) {
        fprintf(stderr, "pastetest: quote in path\n");
        return 0;
    }
    char cmd[2048];
    snprintf(cmd, sizeof cmd,
             "osascript -e 'set the clipboard to (read (POSIX file \"%s\") "
             "as «class PNGf»)' >/dev/null 2>&1",
             file);
    if (system(cmd) != 0) {
        fprintf(stderr, "pastetest: could not put %s on the clipboard\n", file);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        char abs[1024];
        if (argv[1][0] == '/') {
            snprintf(abs, sizeof abs, "%s", argv[1]);
        } else {
            char cwd[768];
            if (!getcwd(cwd, sizeof cwd))
                return 1;
            snprintf(abs, sizeof abs, "%s/%s", cwd, argv[1]);
        }
        if (!put_on_clipboard(abs))
            return 1;
        printf("clipboard  <- %s\n", abs);
    }

    char path[1024];
    if (!paste_image(path, sizeof path)) {
        printf("no image on the clipboard\n");
        return 1;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        printf("FAIL: %s reported but missing\n", path);
        return 1;
    }
    printf("wrote      %s (%lld bytes)\n", path, (long long)st.st_size);

    /* The grab is only useful if it produced something a decoder accepts. */
    static const unsigned char PNG_MAGIC[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    FILE *f = fopen(path, "rb");
    unsigned char head[8] = {0};
    size_t got = f ? fread(head, 1, sizeof head, f) : 0;
    if (f)
        fclose(f);
    if (got != sizeof head || memcmp(head, PNG_MAGIC, sizeof head) != 0) {
        printf("FAIL: not a PNG\n");
        return 1;
    }
    printf("ok         valid PNG\n");
    return 0;
}
