// An image is a block of placeholder cells. Every row of it must reach the
// same column, and none of them may reach the last column of the screen: the
// viewport paints with autowrap off, and tmux hangs a combining mark written
// there on the cell before it, which costs the row its last placeholder.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "image.h"
#include "screenmodel.h"
#include "ui.h"
#include "viewport.h"

static int failures;
static int tap_read = -1;

static void pump(struct screen *s)
{
    fflush(stdout);
    char    buf[1 << 20];
    ssize_t n;
    while ((n = read(tap_read, buf, sizeof buf)) > 0)
        feed(s, buf, (size_t)n);
}

static void set_size(int cols, int rows)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%d", cols);
    setenv("COLUMNS", buf, 1);
    snprintf(buf, sizeof buf, "%d", rows);
    setenv("LINES", buf, 1);
}

static void say(const char *t)
{
    viewport_write(t, strlen(t));
    viewport_write("\n", 1);
}

// Cells of the placeholder codepoint on one screen row, and the column the
// last of them sits in.
static int row_cells(struct screen *s, int r, int *last_col)
{
    int n = 0;
    *last_col = -1;
    for (int c = 0; c < s->cols; c++) {
        const char *cell = s->cell[r][c];
        if (strncmp(cell, "\xf4\x8e\xbb\xae", 4) == 0) {
            n++;
            *last_col = c;
        }
    }
    return n;
}

static void report(struct screen *s, const char *what)
{
    fprintf(stderr, "--- %s (%dx%d)\n", what, s->cols, s->rows);
    for (int r = 0; r < s->rows; r++) {
        int last;
        int n = row_cells(s, r, &last);
        if (n)
            fprintf(stderr, "row %2d: %3d cells, last col %d\n", r, n, last);
    }
}

// Rows of one block must agree; a blank row starts a new block.
static void check_uniform(struct screen *s, const char *what)
{
    int first = -1, first_last = -1;
    for (int r = 0; r < s->rows; r++) {
        int last;
        int n = row_cells(s, r, &last);
        if (!n) {
            first = -1;
            continue;
        }
        if (last >= s->cols - 1) {
            fprintf(stderr, "FAIL %s: row %d reaches the last column (%d of %d)\n",
                    what, r, last, s->cols);
            failures++;
            report(s, what);
            return;
        }
        if (first < 0) {
            first = n;
            first_last = last;
            continue;
        }
        if (n != first || last != first_last) {
            fprintf(stderr, "FAIL %s: row %d has %d cells ending at %d, not %d/%d\n",
                    what, r, n, last, first, first_last);
            failures++;
            report(s, what);
            return;
        }
    }
}

static void place_image(int w, int h)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", 0x414243);
    cJSON_AddNumberToObject(o, "indent", 2);
    cJSON_AddNumberToObject(o, "w", w);
    cJSON_AddNumberToObject(o, "h", h);
    image_placed_load(o);
    cJSON_Delete(o);
}

int main(void)
{
    set_size(80, 30);

    char path[] = "/tmp/mux-imagerowtest-XXXXXX";
    int  wfd = mkstemp(path);
    tap_read = wfd >= 0 ? open(path, O_RDONLY) : -1;
    if (wfd < 0 || tap_read < 0)
        return 1;
    unlink(path);
    fflush(stdout);
    dup2(wfd, STDOUT_FILENO);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);

    ui_init();
    viewport_begin();

    struct screen s;
    screen_init(&s, 30, 80);

    viewport_clear();
    say("before");
    place_image(1404, 1872);        /* portrait: the height is what binds */
    say("between");
    place_image(2400, 600);         /* landscape: the fit runs to the width */
    say("after");
    viewport_paint();
    pump(&s);
    check_uniform(&s, "first paint");
    report(&s, "first paint");

    const int W[] = {66, 40, 100, 66};
    for (int i = 0; i < (int)(sizeof W / sizeof *W); i++) {
        set_size(W[i], 30);
        screen_init(&s, 30, W[i]);
        viewport_forget();
        viewport_paint();
        pump(&s);
        check_uniform(&s, "after resize");
        report(&s, "after resize");
    }

    fflush(stdout);
    fprintf(stderr, failures ? "imagerowtest: FAILURES\n" : "imagerowtest: ok\n");
    return failures ? 1 : 0;
}

/* The loader table wants every kind; only the image one is under test. */
void bash_ran_load(const cJSON *st);
void md_kept_load(const cJSON *st);
void prompt_echo_load(const cJSON *st);
void sidechannel_btw_load(const cJSON *st);
void view_keep_load(const cJSON *st);
void bash_ran_load(const cJSON *st) { (void)st; }
void md_kept_load(const cJSON *st) { (void)st; }
void prompt_echo_load(const cJSON *st) { (void)st; }
void sidechannel_btw_load(const cJSON *st) { (void)st; }
void view_keep_load(const cJSON *st) { (void)st; }
