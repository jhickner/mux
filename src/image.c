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

static char *convert_to_png(const char *path)
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
        return NULL;
    if (!mkdtemp(dir))
        return NULL;

    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s/out.png", dir);

    pid_t pid = fork();
    if (pid < 0) {
        rmdir(dir);
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
        rmdir(dir);
        return NULL;
    }
    char *out = strdup(tmp);
    if (!out) {
        unlink(tmp);
        rmdir(dir);
    }
    return out;
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

int image_show(const char *path, int indent)
{
    if (!image_available() || !path || !*path)
        return 0;

    char *expanded = path_expand_home(path);
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
        discard_temp_png(converted);
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
