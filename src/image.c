#include "image.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "ui.h"

#define TERM_H
static void term_write_n(const char *s, int n) { fwrite(s, 1, (size_t)n, stdout); }
static void term_write(const char *s) { term_write_n(s, (int)strlen(s)); }
static void term_flush(void) { fflush(stdout); }
static void term_move_cursor(int col, int row) { printf("\x1b[%d;%dH", row + 1, col + 1); }

#define KITTY_IMPLEMENTATION
#include "vendor/kitty.h"
#include "text.h"

#define IMAGE_MAX_BYTES (16u * 1024u * 1024u)

static int image_ready;
static int image_ok;
static int max_rows = IMAGE_ROWS_DEFAULT;

void image_set_rows(int rows)
{
    if (rows < IMAGE_ROWS_MIN)
        rows = IMAGE_ROWS_MIN;
    if (rows > IMAGE_ROWS_MAX)
        rows = IMAGE_ROWS_MAX;
    max_rows = rows;
}

int image_rows(void) { return max_rows; }

void image_init(void)
{
    if (image_ready)
        return;
    image_ready = 1;

    if (!isatty(STDOUT_FILENO) || !kg_supported())
        return;
    kg_init();
    if (kg_passthrough())
        kg_tmux_allow_passthrough();
    image_ok = 1;
}

int image_available(void)
{
    image_init();
    return image_ok;
}

static unsigned char *load_file(const char *path, size_t *len)
{
    unsigned char *buf = (unsigned char *)text_slurp(path, IMAGE_MAX_BYTES, len);
    if (buf && *len == 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int is_png(const unsigned char *d, size_t n)
{
    return n > 24 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0;
}

static int png_dims(const unsigned char *d, size_t n, int *w, int *h)
{
    if (!is_png(d, n) || memcmp(d + 12, "IHDR", 4) != 0)
        return 0;
    *w = (int)be32(d + 16);
    *h = (int)be32(d + 20);
    return *w > 0 && *h > 0;
}

#define CACHE_MAX   16
#define PENDING_MAX 4

struct img_cache {
    char     path[4096];
    time_t   mtime;
    uint32_t id;
    int      cols, rows;
};

struct pending {
    int      live;
    uint32_t id;
    pid_t    pid;
    char     tmp[4200];
    char     src[4096];
    time_t   mtime;
};

static struct img_cache cache[CACHE_MAX];
static int cache_n;
static struct pending pending[PENDING_MAX];

static time_t file_mtime(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

static struct img_cache *cache_find(const char *path, time_t mtime)
{
    for (int i = 0; i < cache_n; i++)
        if (cache[i].mtime == mtime && strcmp(cache[i].path, path) == 0)
            return &cache[i];
    return NULL;
}

static void cache_store(const char *path, time_t mtime, uint32_t id, int cols, int rows)
{
    struct img_cache *slot = cache_find(path, mtime);
    if (!slot) {
        if (cache_n < CACHE_MAX)
            slot = &cache[cache_n++];
        else
            slot = &cache[0];
    }
    snprintf(slot->path, sizeof slot->path, "%s", path);
    slot->mtime = mtime;
    slot->id = id;
    slot->cols = cols;
    slot->rows = rows;
}

static pid_t convert_start(const char *path, char *tmp, size_t tmp_sz)
{
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir)
        tmpdir = "/tmp";

    char dir[4096];
    size_t n = strlen(tmpdir);
    while (n > 1 && tmpdir[n - 1] == '/')
        n--;
    if (snprintf(dir, sizeof dir, "%.*s/" APP_NAME "-img-XXXXXX", (int)n, tmpdir) >=
        (int)sizeof dir)
        return -1;
    if (!mkdtemp(dir))
        return -1;

    if ((size_t)snprintf(tmp, tmp_sz, "%s/out.png", dir) >= tmp_sz) {
        rmdir(dir);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        rmdir(dir);
        return -1;
    }
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
        }
        execlp("sips", "sips", "-s", "format", "png", path, "--out", tmp, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static void discard_temp_png(const char *file)
{
    unlink(file);
    const char *slash = strrchr(file, '/');
    if (!slash)
        return;
    char dir[4096];
    size_t n = (size_t)(slash - file);
    if (n < sizeof dir) {
        memcpy(dir, file, n);
        dir[n] = '\0';
        rmdir(dir);
    }
}

static void cell_pixels(int *cw, int *ch, int *rows)
{
    struct winsize ws;
    *cw = 0;
    *ch = 0;
    *rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) {
            *rows = ws.ws_row;
            if (ws.ws_ypixel > 0)
                *ch = ws.ws_ypixel / ws.ws_row;
        }
        if (ws.ws_col > 0 && ws.ws_xpixel > 0)
            *cw = ws.ws_xpixel / ws.ws_col;
    }
    if (*cw <= 0)
        *cw = 8;
    if (*ch <= 0)
        *ch = 16;
}

static uint32_t next_id(void)
{
    static unsigned counter;
    unsigned pid = (unsigned)getpid();
    return (uint32_t)(0x40 | (pid & 0x3F)) << 16 |
           (uint32_t)(0x40 | ((pid >> 6) & 0x3F)) << 8 |
           (uint32_t)(0x40 | (counter++ & 0x3F));
}

static void write_placeholders(uint32_t id, int indent, int cols, int rows)
{
    kg_virtual_place(id, cols, rows);
    kg_placeholder_redraw_begin();
    for (int r = 0; r < rows; r++) {
        for (int i = 0; i < indent; i++)
            ui_put(" ");
        for (int c = 0; c < cols; c++)
            kg_placeholder_cell(id, r, c);
        ui_esc("\x1b[39m");
        ui_put("\n");
    }
    kg_placeholder_redraw_end();
    ui_flush();
}

static int box_size(int indent, int img_w, int img_h, int *cols, int *rows)
{
    int cw, ch, term_rows;
    cell_pixels(&cw, &ch, &term_rows);

    int cols_box = ui_columns() - indent;
    int rows_box = term_rows - 4 < max_rows ? term_rows - 4 : max_rows;
    if (cols_box < 4 || rows_box < 2)
        return 0;
    if (img_w > 0 && img_h > 0)
        kg_fit_cells(img_w, img_h, cw, ch, cols_box, rows_box, cols, rows, NULL, NULL);
    else {
        *cols = cols_box < 48 ? cols_box : 48;
        *rows = rows_box < 8 ? rows_box : 8;
    }
    return *cols > 0 && *rows > 0;
}

static int show_png(const unsigned char *data, size_t len, const char *path, time_t mtime,
                    int indent)
{
    int img_w = 0, img_h = 0;
    if (!png_dims(data, len, &img_w, &img_h))
        return 0;

    int cols, rows;
    if (!box_size(indent, img_w, img_h, &cols, &rows))
        return 0;

    uint32_t id = next_id();
    kg_transmit_png(id, data, len);
    write_placeholders(id, indent, cols, rows);
    if (path)
        cache_store(path, mtime, id, cols, rows);
    return 1;
}

static int start_convert(const char *path, time_t mtime, int indent)
{
    int slot = -1;
    for (int i = 0; i < PENDING_MAX; i++) {
        if (pending[i].live && strcmp(pending[i].src, path) == 0 &&
            pending[i].mtime == mtime) {
            write_placeholders(pending[i].id, indent, 40, 8);
            return 1;
        }
        if (!pending[i].live && slot < 0)
            slot = i;
    }
    if (slot < 0)
        return 0;

    char tmp[4200];
    pid_t pid = convert_start(path, tmp, sizeof tmp);
    if (pid < 0)
        return 0;

    int cols, rows;
    if (!box_size(indent, 0, 0, &cols, &rows)) {
        kill(pid, SIGTERM);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
        discard_temp_png(tmp);
        return 0;
    }

    uint32_t id = next_id();
    pending[slot].live = 1;
    pending[slot].id = id;
    pending[slot].pid = pid;
    snprintf(pending[slot].tmp, sizeof pending[slot].tmp, "%s", tmp);
    snprintf(pending[slot].src, sizeof pending[slot].src, "%s", path);
    pending[slot].mtime = mtime;
    write_placeholders(id, indent, cols, rows);
    cache_store(path, mtime, id, cols, rows);
    return 1;
}

int image_show(const char *path, int indent)
{
    if (!image_available() || !path || !*path)
        return 0;

    char *expanded = path_expand_home(path);
    const char *file = expanded ? expanded : path;
    time_t mtime = file_mtime(file);

    struct img_cache *hit = cache_find(file, mtime);
    if (hit) {
        write_placeholders(hit->id, indent, hit->cols, hit->rows);
        free(expanded);
        return 1;
    }

    size_t len = 0;
    unsigned char *data = load_file(file, &len);
    int ok = 0;
    if (data && is_png(data, len)) {
        ok = show_png(data, len, file, mtime, indent);
    } else if (data) {
        ok = start_convert(file, mtime, indent);
    }
    free(data);
    free(expanded);
    return ok;
}

static void finish_pending(struct pending *p, int status)
{
    p->live = 0;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        size_t len = 0;
        unsigned char *data = load_file(p->tmp, &len);
        if (data)
            kg_transmit_png(p->id, data, len);
        free(data);
    }
    discard_temp_png(p->tmp);
}

void image_poll(void)
{
    for (int i = 0; i < PENDING_MAX; i++) {
        struct pending *p = &pending[i];
        if (!p->live)
            continue;
        int status = 0;
        pid_t r = waitpid(p->pid, &status, WNOHANG);
        if (r == 0)
            continue;
        if (r == p->pid)
            finish_pending(p, status);
        else {
            p->live = 0;
            discard_temp_png(p->tmp);
        }
    }
}

void image_wait(void)
{
    for (int i = 0; i < PENDING_MAX; i++) {
        struct pending *p = &pending[i];
        if (!p->live)
            continue;
        int status = 0;
        pid_t r;
        while ((r = waitpid(p->pid, &status, 0)) < 0 && errno == EINTR)
            ;
        if (r == p->pid)
            finish_pending(p, status);
        else {
            p->live = 0;
            discard_temp_png(p->tmp);
        }
    }
}
