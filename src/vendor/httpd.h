/**
 * httpd.h — single-header static-file server for build artifacts (C)
 *
 * Serves one directory read-only over HTTP so the agent can hand back a link
 * instead of a file: a PDF it typeset, an HTML report, a screenshot. GET and
 * HEAD only, one connection at a time, on its own thread.
 *
 * The point is a link that opens on the phone, so the default binding is the
 * machine's Tailscale address (see httpd_tailscale_ip) — reachable from the
 * user's own devices and from nothing else. An unguessable token prefixes every
 * path, so a link stays private even where the tailnet is shared.
 *
 * USAGE (stb style):
 *
 *   // In exactly ONE .c file:
 *   #define HTTPD_IMPLEMENTATION
 *   #include "httpd.h"
 *
 *   httpd *h = httpd_start("/path/to/artifacts", httpd_tailscale_ip(), 8787, tok);
 *   // ... links look like http://100.x.y.z:8787/<tok>/report.pdf
 *   httpd_stop(h);
 *
 * LINK: -lpthread
 *
 * Symlinks inside the root are followed on purpose: publishing a file means
 * linking it in, not copying it. ".." is rejected, so nothing outside the root
 * is reachable except through a symlink deliberately placed there.
 */

#ifndef HTTPD_H
#define HTTPD_H

typedef struct httpd httpd;

/* Serve `root` on `bind_ip`:`port`. `bind_ip` NULL/"" binds every interface —
 * prefer httpd_tailscale_ip() or "127.0.0.1". `token`, when non-empty, must
 * prefix every request path. Returns NULL if the socket cannot be bound (the
 * port is taken, or the address is not up). */
httpd *httpd_start(const char *root, const char *bind_ip, int port, const char *token);

/* Stop serving and free. Safe with NULL. */
void httpd_stop(httpd *h);

/* Free the port and return at once, without waiting for the serving thread to
 * notice or freeing the server. For shutting down on the way out of the
 * process, where waiting is the only cost and there is nothing to wait for. */
void httpd_close(httpd *h);

/* This machine's Tailscale address (the 100.64.0.0/10 CGNAT range Tailscale
 * assigns), or NULL when the tailnet is not up. Points at a static buffer. */
const char *httpd_tailscale_ip(void);

#endif /* HTTPD_H */

/* ======================================================================== */
/*   IMPLEMENTATION                                                          */
/* ======================================================================== */
#ifdef HTTPD_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

#define HD_REQ_MAX 8192

struct httpd {
    int fd;
    int stop;
    char root[512];
    char token[64];
    pthread_t thread;
};

const char *httpd_tailscale_ip(void) {
    static char ip[INET_ADDRSTRLEN];
    if (ip[0]) return ip;

    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0) return NULL;
    for (struct ifaddrs *p = ifs; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        struct sockaddr_in *a = (struct sockaddr_in *)p->ifa_addr;
        /* 100.64.0.0/10 — the shared-address range Tailscale hands out. */
        unsigned long h = ntohl(a->sin_addr.s_addr);
        if ((h & 0xffc00000ul) != 0x64400000ul) continue;
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
        break;
    }
    freeifaddrs(ifs);
    return ip[0] ? ip : NULL;
}

static const char *hd_mime(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    static const struct { const char *ext, *type; } types[] = {
        { "html", "text/html; charset=utf-8" },
        { "htm",  "text/html; charset=utf-8" },
        { "css",  "text/css; charset=utf-8" },
        { "js",   "text/javascript; charset=utf-8" },
        { "json", "application/json; charset=utf-8" },
        { "svg",  "image/svg+xml" },
        { "png",  "image/png" },
        { "jpg",  "image/jpeg" },
        { "jpeg", "image/jpeg" },
        { "gif",  "image/gif" },
        { "webp", "image/webp" },
        { "pdf",  "application/pdf" },
        { "txt",  "text/plain; charset=utf-8" },
        { "md",   "text/plain; charset=utf-8" },
        { "log",  "text/plain; charset=utf-8" },
        { "csv",  "text/csv; charset=utf-8" },
        { "mp3",  "audio/mpeg" },
        { "mp4",  "video/mp4" },
        { "wav",  "audio/wav" },
        { "zip",  "application/zip" },
        { NULL, NULL },
    };
    for (int i = 0; types[i].ext; i++)
        if (!strcasecmp(dot + 1, types[i].ext)) return types[i].type;
    return "application/octet-stream";
}

static void hd_write(int fd, const char *buf, size_t n) {
    for (size_t off = 0; off < n; ) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; return; }
        off += (size_t)w;
    }
}

static void hd_status(int fd, const char *status, const char *body) {
    char head[512];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: text/plain; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     status, strlen(body));
    hd_write(fd, head, (size_t)n);
    hd_write(fd, body, strlen(body));
}

/* %XX and '+' back to bytes, in place. */
static void hd_url_decode(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            char *end = NULL;
            long v = strtol(hex, &end, 16);
            if (end == hex + 2) { *o++ = (char)v; p += 2; continue; }
        }
        *o++ = (*p == '+') ? ' ' : *p;
    }
    *o = '\0';
}

/* Enough escaping for a filename dropped into an href or link text. */
static void hd_html_escape(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = in; *p && o + 7 < n; p++) {
        const char *rep = *p == '&' ? "&amp;" : *p == '<' ? "&lt;"
                        : *p == '>' ? "&gt;" : *p == '"' ? "&quot;" : NULL;
        if (rep) { size_t l = strlen(rep); memcpy(out + o, rep, l); o += l; }
        else out[o++] = *p;
    }
    out[o] = '\0';
}

static void hd_send_file(int fd, const char *path, off_t size, int head_only) {
    int in = open(path, O_RDONLY);
    if (in < 0) { hd_status(fd, "404 Not Found", "not found\n"); return; }

    char header[512];
    int n = snprintf(header, sizeof header,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lld\r\n"
                     "Connection: close\r\n\r\n",
                     hd_mime(path), (long long)size);
    hd_write(fd, header, (size_t)n);

    if (!head_only) {
        char buf[64 * 1024];
        ssize_t r;
        while ((r = read(in, buf, sizeof buf)) > 0) hd_write(fd, buf, (size_t)r);
    }
    close(in);
}

/* Newest first: what you just built is what you are looking for. */
static int hd_by_mtime(const void *a, const void *b) {
    const struct { char name[256]; off_t size; time_t mtime; } *x = a, *y = b;
    return x->mtime < y->mtime ? 1 : x->mtime > y->mtime ? -1 : 0;
}

static void hd_send_index(int fd, httpd *h, const char *dir, const char *urlpath,
                          int head_only) {
    DIR *d = opendir(dir);
    if (!d) { hd_status(fd, "404 Not Found", "not found\n"); return; }

    struct entry { char name[256]; off_t size; time_t mtime; } *ents = NULL;
    int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            struct entry *grown = realloc(ents, (size_t)cap * sizeof *ents);
            if (!grown) break;
            ents = grown;
        }
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        snprintf(ents[n].name, sizeof ents[n].name, "%s%s", de->d_name,
                 S_ISDIR(st.st_mode) ? "/" : "");
        ents[n].size = S_ISDIR(st.st_mode) ? 0 : st.st_size;
        ents[n].mtime = st.st_mtime;
        n++;
    }
    closedir(d);
    if (ents) qsort(ents, (size_t)n, sizeof *ents, hd_by_mtime);

    char *body = NULL;
    size_t len = 0, bcap = 0;
    #define HD_APPEND(fmt, ...) do {                                        \
        char chunk[1400];                                                   \
        int c = snprintf(chunk, sizeof chunk, fmt, __VA_ARGS__);            \
        if (c > 0 && len + (size_t)c + 1 > bcap) {                          \
            bcap = (len + (size_t)c + 1) * 2;                               \
            char *g = realloc(body, bcap);                                  \
            if (!g) break;                                                  \
            body = g;                                                       \
        }                                                                   \
        if (c > 0 && body) { memcpy(body + len, chunk, (size_t)c); len += (size_t)c; body[len] = '\0'; } \
    } while (0)

    HD_APPEND("<!doctype html><meta charset=utf-8>"
              "<meta name=viewport content='width=device-width,initial-scale=1'>"
              "<title>artifacts</title>"
              "<style>body{font:16px/1.6 -apple-system,system-ui,sans-serif;"
              "margin:2rem auto;max-width:44rem;padding:0 1rem}"
              "a{text-decoration:none}li{margin:.25rem 0}"
              "small{color:#777;margin-left:.5rem}</style>"
              "<h2>%s</h2><ul>", "artifacts");
    for (int i = 0; i < n; i++) {
        char esc[1024];
        hd_html_escape(ents[i].name, esc, sizeof esc);
        char when[32];
        struct tm tm;
        localtime_r(&ents[i].mtime, &tm);
        strftime(when, sizeof when, "%b %d %H:%M", &tm);
        if (ents[i].size)
            HD_APPEND("<li><a href='%s%s'>%s</a><small>%.1f KB · %s</small></li>",
                      urlpath, esc, esc, (double)ents[i].size / 1024.0, when);
        else
            HD_APPEND("<li><a href='%s%s'>%s</a><small>%s</small></li>",
                      urlpath, esc, esc, when);
    }
    HD_APPEND("</ul>%s", n ? "" : "<p>nothing here yet</p>");
    #undef HD_APPEND
    free(ents);

    const char *out = body ? body : "";
    char header[256];
    int c = snprintf(header, sizeof header,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n", strlen(out));
    hd_write(fd, header, (size_t)c);
    if (!head_only) hd_write(fd, out, strlen(out));
    free(body);
    (void)h;
}

static void hd_serve(httpd *h, int fd) {
    char req[HD_REQ_MAX];
    ssize_t r = read(fd, req, sizeof req - 1);
    if (r <= 0) return;
    req[r] = '\0';

    char method[8] = {0}, target[2048] = {0};
    if (sscanf(req, "%7s %2047s", method, target) != 2) {
        hd_status(fd, "400 Bad Request", "bad request\n");
        return;
    }
    int head_only = !strcmp(method, "HEAD");
    if (strcmp(method, "GET") && !head_only) {
        hd_status(fd, "405 Method Not Allowed", "GET only\n");
        return;
    }

    char *q = strchr(target, '?');
    if (q) *q = '\0';
    hd_url_decode(target);

    const char *path = target;
    if (*path != '/') { hd_status(fd, "400 Bad Request", "bad request\n"); return; }
    path++;

    /* The token is the whole access check: no token, no answer. */
    if (h->token[0]) {
        size_t tl = strlen(h->token);
        if (strncmp(path, h->token, tl) != 0 || (path[tl] && path[tl] != '/')) {
            hd_status(fd, "404 Not Found", "not found\n");
            return;
        }
        path += tl;
        if (*path == '/') path++;
    }

    /* Nothing may climb out of the root. */
    if (strstr(path, "..")) {
        hd_status(fd, "403 Forbidden", "forbidden\n");
        return;
    }

    char full[1024];
    snprintf(full, sizeof full, "%s/%s", h->root, path);

    struct stat st;
    if (stat(full, &st) != 0) {
        hd_status(fd, "404 Not Found", "not found\n");
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        /* A directory link needs its trailing slash or every href below it
         * resolves against the parent. */
        size_t tlen = strlen(target);
        if (tlen && target[tlen - 1] != '/') {
            char head[2200];
            int c = snprintf(head, sizeof head,
                             "HTTP/1.1 301 Moved Permanently\r\n"
                             "Location: %s/\r\n"
                             "Content-Length: 0\r\n"
                             "Connection: close\r\n\r\n", target);
            hd_write(fd, head, (size_t)c);
            return;
        }
        hd_send_index(fd, h, full, target, head_only);
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        hd_status(fd, "403 Forbidden", "forbidden\n");
        return;
    }
    hd_send_file(fd, full, st.st_size, head_only);
}

static void *hd_loop(void *ud) {
    httpd *h = ud;
    for (;;) {
        int c = accept(h->fd, NULL, NULL);
        if (h->stop) { if (c >= 0) close(c); break; }
        if (c < 0) {
            if (errno == EINTR) continue;
            break;
        }
        /* A stalled client must not wedge the only serving thread. */
        struct timeval tv = { 10, 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        hd_serve(h, c);
        close(c);
    }
    return NULL;
}

httpd *httpd_start(const char *root, const char *bind_ip, int port, const char *token) {
    if (!root || !*root || port <= 0 || port > 65535) return NULL;

    httpd *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    snprintf(h->root, sizeof h->root, "%s", root);
    if (token) snprintf(h->token, sizeof h->token, "%s", token);

    h->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (h->fd < 0) { free(h); return NULL; }
    int one = 1;
    setsockopt(h->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind_ip && *bind_ip && inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        close(h->fd); free(h); return NULL;
    }
    if (bind(h->fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(h->fd, 8) != 0) {
        close(h->fd); free(h); return NULL;
    }
    if (pthread_create(&h->thread, NULL, hd_loop, h) != 0) {
        close(h->fd); free(h); return NULL;
    }
    return h;
}

void httpd_close(httpd *h) {
    if (!h) return;
    h->stop = 1;
    shutdown(h->fd, SHUT_RDWR);
    close(h->fd);
    pthread_detach(h->thread);
    /* h is deliberately not freed: the serving thread may still be inside it. */
}

void httpd_stop(httpd *h) {
    if (!h) return;
    h->stop = 1;
    shutdown(h->fd, SHUT_RDWR);
    close(h->fd);
    pthread_join(h->thread, NULL);
    free(h);
}

#endif /* HTTPD_IMPLEMENTATION */
