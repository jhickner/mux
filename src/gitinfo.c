#include "gitinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define TTL_MS 3000

static struct gitinfo cache;
static char           cached_dir[4096];
static double         read_at;

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

static int shell_quote(const char *s, char *out, size_t size)
{
    size_t n = 0;
    if (size < 3)
        return 0;
    out[n++] = '\'';
    for (; *s; s++) {
        const char *piece = *s == '\'' ? "'\\''" : NULL;
        size_t need = piece ? 4 : 1;
        if (n + need + 2 > size)
            return 0;
        if (piece) {
            memcpy(out + n, piece, need);
            n += need;
        } else {
            out[n++] = *s;
        }
    }
    out[n++] = '\'';
    out[n] = '\0';
    return 1;
}

static void chomp(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

static void parse_shortstat(const char *line, struct gitinfo *g)
{
    for (const char *p = line; *p; p++) {
        if (*p < '0' || *p > '9')
            continue;
        long value = strtol(p, (char **)&p, 10);
        while (*p == ' ')
            p++;
        if (strncmp(p, "insertion", 9) == 0)
            g->added = value;
        else if (strncmp(p, "deletion", 8) == 0)
            g->removed = value;
        if (!*p)
            break;
    }
}

static void reread(const char *dir, struct gitinfo *g)
{
    memset(g, 0, sizeof *g);

    char quoted[4200];
    if (!shell_quote(dir, quoted, sizeof quoted))
        return;

    char cmd[17000];
    snprintf(cmd, sizeof cmd,
             "git -C %s rev-parse --abbrev-ref HEAD 2>/dev/null; echo @; "
             "git -C %s rev-parse --short HEAD 2>/dev/null; echo @; "
             "git -C %s diff --shortstat HEAD 2>/dev/null; echo @; "
             "git -C %s status --porcelain 2>/dev/null",
             quoted, quoted, quoted, quoted);

    FILE *f = popen(cmd, "r");
    if (!f)
        return;

    char line[1024];
    int  section = 0;
    while (fgets(line, sizeof line, f)) {
        chomp(line);
        if (strcmp(line, "@") == 0) {
            section++;
            continue;
        }
        if (!*line)
            continue;
        switch (section) {
        case 0:
            if (strcmp(line, "HEAD") != 0)
                snprintf(g->branch, sizeof g->branch, "%s", line);
            g->repo = 1;
            break;
        case 1:
            snprintf(g->sha, sizeof g->sha, "%s", line);
            g->repo = 1;
            break;
        case 2:
            parse_shortstat(line, g);
            break;
        default:
            if (strncmp(line, "??", 2) == 0)
                g->untracked = 1;
            else
                g->dirty = 1;
            break;
        }
    }
    pclose(f);
}

void gitinfo_forget(void) { read_at = 0; }

const struct gitinfo *gitinfo_get(const char *dir)
{
    if (!dir || !*dir) {
        memset(&cache, 0, sizeof cache);
        return &cache;
    }
    double t = now_ms();
    if (strcmp(dir, cached_dir) != 0 || read_at == 0 || t - read_at >= TTL_MS) {
        snprintf(cached_dir, sizeof cached_dir, "%s", dir);
        reread(dir, &cache);
        read_at = t;
    }
    return &cache;
}
