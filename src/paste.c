#include "paste.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app.h"

#define APP APP_NAME

/* Kept rather than written to a temp dir: a path in an earlier message stays
 * valid when the conversation is resumed days later. */
static int paste_dir(char *out, size_t size)
{
    const char *home = getenv("HOME");
    if (!home)
        return 0;
    char base[1024];
    if ((size_t)snprintf(base, sizeof base, "%s/.config/%s", home, APP) >= sizeof base)
        return 0;
    mkdir(base, 0700);
    if ((size_t)snprintf(out, size, "%s/pastes", base) >= size)
        return 0;
    mkdir(out, 0700);
    return 1;
}

/* A name no existing paste holds. Same-second pastes get a counter. */
static int paste_path(char *out, size_t size, const char *dir)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);

    for (int n = 0; n < 1000; n++) {
        int len = n == 0 ? snprintf(out, size, "%s/paste-%s.png", dir, stamp)
                         : snprintf(out, size, "%s/paste-%s-%d.png", dir, stamp, n);
        if (len < 0 || (size_t)len >= size)
            return 0;
        if (access(out, F_OK) != 0)
            return 1;
    }
    return 0;
}

int paste_image(char *out, size_t size)
{
#ifndef __APPLE__
    (void)out;
    (void)size;
    return 0;
#else
    char dir[1024];
    if (!paste_dir(dir, sizeof dir) || !paste_path(out, size, dir))
        return 0;
    /* The path is interpolated into a shell command and an AppleScript string;
     * a quote anywhere in $HOME would break out of both. */
    if (strchr(out, '\'') || strchr(out, '"') || strchr(out, '\\'))
        return 0;

    /* «class PNGf» is AppleScript's raw four-char pasteboard type. Asking for
     * it coerces whatever image flavour is on the pasteboard — a screenshot
     * arrives as TIFF — so this is the only form worth trying. Coercion fails
     * outright when there is no image, before the file is opened, which is why
     * a miss leaves nothing behind. */
    char cmd[2048];
    int len = snprintf(cmd, sizeof cmd,
                       "osascript"
                       " -e 'set d to (the clipboard as «class PNGf»)'"
                       " -e 'set f to POSIX file \"%s\"'"
                       " -e 'set h to open for access f with write permission'"
                       " -e 'try'"
                       " -e 'write d to h'"
                       " -e 'end try'"
                       " -e 'close access h'"
                       " >/dev/null 2>&1",
                       out);
    if (len < 0 || (size_t)len >= sizeof cmd)
        return 0;
    if (system(cmd) != 0)
        return 0;

    struct stat st;
    if (stat(out, &st) != 0 || st.st_size == 0) {
        unlink(out);
        return 0;
    }
    return 1;
#endif
}

#define CHUNK 4096

char *paste_text(void)
{
#ifndef __APPLE__
    return NULL;
#else
    FILE *f = popen("pbpaste 2>/dev/null", "r");
    if (!f)
        return NULL;

    char  *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len + CHUNK + 1 > cap) {
            size_t want = cap ? cap * 2 : CHUNK * 2 + 1;
            char  *grown = realloc(buf, want);
            if (!grown) {
                free(buf);
                pclose(f);
                return NULL;
            }
            buf = grown;
            cap = want;
        }
        size_t n = fread(buf + len, 1, CHUNK, f);
        len += n;
        if (n < CHUNK)
            break;
    }
    pclose(f);

    if (!buf)
        return NULL;
    buf[len] = '\0';
    if (len == 0) {
        free(buf);
        return NULL;
    }
    return buf;
#endif
}
