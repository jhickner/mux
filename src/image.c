#include "image.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

#define IMG_MAX_BYTES (16u * 1024u * 1024u)

static int img_ready;
static int img_ok;
static int max_rows = IMG_ROWS_DEFAULT;

void img_set_rows(int rows)
{
    if (rows < IMG_ROWS_MIN)
        rows = IMG_ROWS_MIN;
    if (rows > IMG_ROWS_MAX)
        rows = IMG_ROWS_MAX;
    max_rows = rows;
}

int img_rows(void) { return max_rows; }

void img_init(void)
{
    if (img_ready)
        return;
    img_ready = 1;

    if (!isatty(STDOUT_FILENO) || !kg_supported())
        return;
    kg_init();
    if (kg_passthrough())
        kg_tmux_allow_passthrough();
    img_ok = 1;
}

int img_available(void)
{
    img_init();
    return img_ok;
}

static char *expand_home(const char *path)
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

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (unsigned long long)st.st_size > IMG_MAX_BYTES) {
        fclose(f);
        return NULL;
    }

    size_t n = (size_t)st.st_size;
    unsigned char *buf = malloc(n);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, n, f) != n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = n;
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

static char *convert_to_png(const char *path)
{
    char tmp[] = "/tmp/" APP_NAME "-img-XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0)
        return NULL;
    close(fd);

    pid_t pid = fork();
    if (pid < 0) {
        unlink(tmp);
        return NULL;
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

    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        unlink(tmp);
        return NULL;
    }
    return strdup(tmp);
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

int img_show(const char *path, int indent)
{
    if (!img_available() || !path || !*path)
        return 0;

    char *expanded = expand_home(path);
    const char *file = expanded ? expanded : path;

    size_t len = 0;
    unsigned char *data = load_file(file, &len);
    char *converted = NULL;

    if (data && !is_png(data, len)) {
        free(data);
        data = NULL;
        if ((converted = convert_to_png(file)) != NULL)
            data = load_file(converted, &len);
    }

    free(expanded);
    if (converted) {
        unlink(converted);
        free(converted);
    }
    if (!data)
        return 0;

    int img_w = 0, img_h = 0;
    if (!png_dims(data, len, &img_w, &img_h)) {
        free(data);
        return 0;
    }

    int cw, ch, term_rows;
    cell_pixels(&cw, &ch, &term_rows);

    int cols_box = ui_columns() - indent;
    int rows_box = term_rows - 4 < max_rows ? term_rows - 4 : max_rows;
    if (cols_box < 4 || rows_box < 2) {
        free(data);
        return 0;
    }

    int cols, rows;
    kg_fit_cells(img_w, img_h, cw, ch, cols_box, rows_box, &cols, &rows, NULL, NULL);

    uint32_t id = next_id();
    kg_transmit_png(id, data, len);
    free(data);
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
    return 1;
}
