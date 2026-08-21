#include "text.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "app.h"

void text_one_line(const char *in, char *out, size_t size)
{
    size_t o = 0;
    int space = 0;
    for (const char *p = in; *p && o + 1 < size; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (o == 0 || space)
                continue;
            space = 1;
            out[o++] = ' ';
            continue;
        }
        space = 0;
        out[o++] = (char)c;
    }
    while (o > 0 && out[o - 1] == ' ')
        o--;
    out[o] = '\0';
}

void text_block(const char *in, char *out, size_t size)
{
    if (!size)
        return;

    size_t o = 0, keep = 0;
    int    seen = 0;
    for (const char *p = in; *p && o + 1 < size; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r')
            continue;
        if (c == '\n') {
            if (!seen)
                continue;
            o = keep;
            if (o + 1 < size)
                out[o++] = '\n';
            continue;
        }
        if (c == '\t')
            c = ' ';
        if (c == ' ' && !seen)
            continue;
        out[o++] = (char)c;
        if (c != ' ') {
            keep = o;
            seen = 1;
        }
    }
    out[keep] = '\0';
}

void text_chomp(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

char *text_slurp(const char *path, size_t max_bytes, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (max_bytes && (unsigned long long)st.st_size > max_bytes)) {
        fclose(f);
        return NULL;
    }

    size_t want = (size_t)st.st_size;
    char *buf = malloc(want + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, want, f);
    fclose(f);
    buf[got] = '\0';
    if (len_out)
        *len_out = got;
    return buf;
}

int text_fuzzy_score(const char *name, const char *q)
{
    int score = 0, ni = 0, streak = 0;

    for (int qi = 0; q[qi]; qi++) {
        int qc = tolower((unsigned char)q[qi]);
        int found = 0;
        while (name[ni]) {
            char prev = ni ? name[ni - 1] : '/';
            int  nc = tolower((unsigned char)name[ni]);
            ni++;
            if (nc == qc) {
                streak++;
                score += 1 + streak;
                if (prev == '/' || prev == '-' || prev == '_' || prev == '.' || prev == ':')
                    score += 4;
                found = 1;
                break;
            }
            streak = 0;
        }
        if (!found)
            return -1;
    }
    return score;
}

size_t text_utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int text_shell_quote(const char *s, char *out, size_t size)
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

double now_seconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

int path_config_dir(char *out, size_t size)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        return 0;
    if ((size_t)snprintf(out, size, "%s/" APP_CONFIG, home) >= size)
        return 0;

    // Every component: a fresh account has no ~/.config either.
    for (char *p = out + strlen(home) + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        mkdir(out, 0700);
        *p = '/';
    }
    mkdir(out, 0700);
    return 1;
}

int path_config_file(char *out, size_t size, const char *leaf)
{
    char dir[4096];
    if (!path_config_dir(dir, sizeof dir))
        return 0;
    if ((size_t)snprintf(out, size, "%s/%s", dir, leaf) >= size)
        return 0;
    return 1;
}

void path_home_relative(const char *dir, char *out, size_t size)
{
    if (!dir) {
        if (size)
            out[0] = '\0';
        return;
    }
    const char *home = getenv("HOME");
    size_t n = home ? strlen(home) : 0;
    if (n && strncmp(dir, home, n) == 0 && (dir[n] == '\0' || dir[n] == '/'))
        snprintf(out, size, "~%s", dir + n);
    else
        snprintf(out, size, "%s", dir);
}

char *path_expand_home(const char *path)
{
    const char *home = getenv("HOME");
    if (path[0] != '~' || (path[1] && path[1] != '/') || !home)
        return NULL;
    size_t n = strlen(home) + strlen(path);
    char *out = malloc(n + 1);
    if (out)
        snprintf(out, n + 1, "%s%s", home, path + 1);
    return out;
}
