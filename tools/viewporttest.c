// The viewport paints every cell of every row it owns, so what is on screen
// after a paint is fully determined by what mux wrote — no guess about how the
// terminal reflowed, and nothing left over from the paint before. This asserts
// that, including across the resizes the old block model could not survive.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ui.h"
#include "viewport.h"

#define ROWS_MAX 64
#define COLS_MAX 256

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

struct screen {
    int  rows, cols;
    char cell[ROWS_MAX][COLS_MAX + 1];
    int  cur_r, cur_c;
};

static void screen_init(struct screen *s, int rows, int cols)
{
    memset(s, 0, sizeof *s);
    s->rows = rows;
    s->cols = cols;
    for (int r = 0; r < rows; r++)
        memset(s->cell[r], ' ', (size_t)cols);
}

// Only what the viewport emits: absolute placement, erase-to-end-of-line, and
// text. Anything else would be a change this model has to learn about.
static void feed(struct screen *s, const char *p, size_t n)
{
    for (size_t i = 0; i < n;) {
        if (p[i] == 0x1b && i + 1 < n && p[i + 1] == '[') {
            size_t j = i + 2;
            int    args[2] = {0, 0}, argc = 0;
            int    priv = j < n && p[j] == '?';
            if (priv)
                j++;
            while (j < n) {
                if (p[j] >= '0' && p[j] <= '9') {
                    if (!argc)
                        argc = 1;
                    if (argc <= 2)
                        args[argc - 1] = args[argc - 1] * 10 + (p[j] - '0');
                    j++;
                } else if (p[j] == ';') {
                    argc = argc < 2 ? argc + 1 : argc;
                    j++;
                } else {
                    break;
                }
            }
            if (j < n && !priv) {
                if (p[j] == 'H') {
                    s->cur_r = (argc > 0 && args[0] > 0 ? args[0] : 1) - 1;
                    s->cur_c = (argc > 1 && args[1] > 0 ? args[1] : 1) - 1;
                } else if (p[j] == 'K' && s->cur_r < s->rows) {
                    for (int c = s->cur_c; c < s->cols; c++)
                        s->cell[s->cur_r][c] = ' ';
                }
            }
            i = j < n ? j + 1 : n;
            continue;
        }
        if (p[i] == 0x1b) {
            // OSC and friends: skip to the terminator.
            size_t j = i + 1;
            while (j < n && p[j] != 0x07 && !(p[j] == 0x1b && j > i + 1))
                j++;
            i = j < n ? j + 1 : n;
            continue;
        }
        if (s->cur_r >= 0 && s->cur_r < s->rows && s->cur_c >= 0 && s->cur_c < s->cols)
            s->cell[s->cur_r][s->cur_c] = p[i];
        s->cur_c++;
        i++;
    }
}

static int tap_read = -1;

static void pump(struct screen *s)
{
    fflush(stdout);
    char    buf[65536];
    ssize_t n;
    while ((n = read(tap_read, buf, sizeof buf)) > 0)
        feed(s, buf, (size_t)n);
}

static void drop(void)
{
    fflush(stdout);
    char    buf[65536];
    while (read(tap_read, buf, sizeof buf) > 0)
        ;
}

static int count_on_screen(const struct screen *s, const char *needle)
{
    int hits = 0;
    for (int r = 0; r < s->rows; r++) {
        char row[COLS_MAX + 1];
        memcpy(row, s->cell[r], (size_t)s->cols);
        row[s->cols] = '\0';
        if (strstr(row, needle))
            hits++;
    }
    return hits;
}

static void say(const char *text)
{
    viewport_write(text, strlen(text));
    viewport_write("\n", 1);
}

static void set_size(int cols, int rows)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%d", cols);
    setenv("COLUMNS", buf, 1);
    snprintf(buf, sizeof buf, "%d", rows);
    setenv("LINES", buf, 1);
}

static void chrome(const char *a, const char *b)
{
    char *rows[2];
    rows[0] = (char *)a;
    rows[1] = (char *)b;
    viewport_chrome(rows, b ? 2 : 1, 0, -1);
}

// The whole point: a resize cannot leave a copy of the chrome behind, because
// the next paint writes every row of the screen from the transcript we hold.
static void check_resize_strands_nothing(struct screen *s)
{
    viewport_clear();
    for (int i = 0; i < 40; i++) {
        char line[64];
        snprintf(line, sizeof line, "transcript %d", i);
        say(line);
    }
    chrome("CHROME-sticky", "CHROME-prompt");

    const int W[] = {80, 20, 34, 52, 100, 24};
    for (int i = 0; i < (int)(sizeof W / sizeof *W); i++) {
        set_size(W[i], 24);
        drop();
        screen_init(s, 24, W[i]);
        viewport_paint();
        pump(s);

        if (count_on_screen(s, "CHROME-prompt") != 1)
            fail("the chrome is on screen exactly once after a resize");
        if (count_on_screen(s, "transcript 39") != 1)
            fail("the newest transcript row is on screen exactly once");
        if (count_on_screen(s, "transcript 0") != 0)
            fail("a row far above the window is not on screen");
    }
}

// A transcript shorter than the window rests on the bottom of it, against the
// chrome, rather than hanging from the top of an otherwise empty screen.
static void check_bottom_up(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);
    say("first");
    say("second");
    say("third");
    chrome("CHROME-prompt", NULL);

    drop();
    screen_init(s, 24, 80);
    viewport_paint();
    pump(s);

    // 24 rows, 1 of chrome: the chrome is row 24 and the three rows sit on
    // 21, 22, 23 rather than 1, 2, 3.
    if (!strstr(s->cell[23], "CHROME-prompt"))
        fail("the chrome is on the last row");
    if (!strstr(s->cell[22], "third"))
        fail("the newest row is directly above the chrome");
    if (!strstr(s->cell[20], "first"))
        fail("the oldest row is pushed down to meet it");
    for (int r = 0; r < 20; r++)
        if (strstr(s->cell[r], "first") || strstr(s->cell[r], "third"))
            fail("nothing is painted at the top of an unfilled screen");
}

// Rows narrower than the screen are one screen row each, so the window shows
// exactly the tail that fits above the chrome.
static void check_tail(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);
    for (int i = 0; i < 100; i++) {
        char line[64];
        snprintf(line, sizeof line, "row %d", i);
        say(line);
    }
    chrome("CHROME-prompt", NULL);

    drop();
    screen_init(s, 24, 80);
    viewport_paint();
    pump(s);

    // 24 rows, 1 of chrome, so the last 23 transcript rows: 77..99.
    if (count_on_screen(s, "row 99") != 1)
        fail("the newest row is shown");
    if (count_on_screen(s, "row 77") != 1)
        fail("the oldest row that fits is shown");
    if (count_on_screen(s, "row 76") != 0)
        fail("the row above the window is not shown");
}

static void check_scroll(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);
    for (int i = 0; i < 100; i++) {
        char line[64];
        snprintf(line, sizeof line, "row %d", i);
        say(line);
    }
    chrome("CHROME-prompt", NULL);

    drop();
    screen_init(s, 24, 80);
    viewport_scroll(10);
    pump(s);
    if (count_on_screen(s, "row 89") != 1)
        fail("scrolling up moves the window back by that many rows");
    if (count_on_screen(s, "row 99") != 0)
        fail("scrolling up drops the newest rows off the bottom");

    drop();
    screen_init(s, 24, 80);
    viewport_scroll_end();
    pump(s);
    if (count_on_screen(s, "row 99") != 1)
        fail("scrolling back to the end shows the newest row again");

    // Output arriving while scrolled up follows the tail again.
    viewport_scroll(10);
    drop();
    screen_init(s, 24, 80);
    say("newest");
    viewport_paint();
    pump(s);
    if (count_on_screen(s, "newest") != 1)
        fail("new output returns the window to the tail");
}

// A kept entry is drawn again at the new width, so it lays out for the width
// the screen has rather than keeping rows measured for a width the screen no
// longer has. Raw output has no renderer and keeps the rows it arrived with.
static int rendered_at;

static void width_render(void *ud, int cols)
{
    (void)ud;
    rendered_at = cols;
    ui_printf("WIDTH-%d", cols);
    ui_put("\n");
}

static void check_reflow(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);

    rendered_at = 0;
    viewport_item_begin(width_render, NULL, NULL);
    width_render(NULL, 80);
    viewport_item_end();

    drop();
    screen_init(s, 24, 80);
    viewport_paint();
    pump(s);
    if (count_on_screen(s, "WIDTH-80") != 1)
        fail("a kept entry shows what it rendered at the first width");

    // Narrower: the entry is asked to render again rather than being chopped.
    set_size(48, 24);
    drop();
    screen_init(s, 24, 48);
    viewport_paint();
    pump(s);
    if (rendered_at != 48)
        fail("a resize re-renders a kept entry at the new width");
    if (count_on_screen(s, "WIDTH-48") != 1)
        fail("the re-rendered entry is what is shown");
    if (count_on_screen(s, "WIDTH-80") != 0)
        fail("the entry laid out for the old width is gone");

    // Raw output has no renderer, so it keeps the rows it was written with.
    viewport_clear();
    set_size(80, 24);
    say("RAW-row");
    set_size(48, 24);
    drop();
    screen_init(s, 24, 48);
    viewport_paint();
    pump(s);
    if (count_on_screen(s, "RAW-row") != 1)
        fail("raw output survives a resize even without a renderer");
}

// A row wider than the screen is painted across several screen rows rather
// than clipped, so narrowing the pane does not hide text.
static void check_soft_wrap(struct screen *s)
{
    viewport_clear();
    set_size(20, 24);
    // Exactly three screen rows at this width, so each marker lands whole.
    say("aaaaaaaaaaaaaaaaaaaa"
        "bbbbbbbbbbbbbbbbbbbb"
        "cccccccccccccccccccc");
    chrome("CHROME-prompt", NULL);

    drop();
    screen_init(s, 24, 20);
    viewport_paint();
    pump(s);

    if (count_on_screen(s, "aaaaaaaaaaaaaaaaaaaa") != 1)
        fail("the head of a wrapped row is shown");
    if (count_on_screen(s, "bbbbbbbbbbbbbbbbbbbb") != 1)
        fail("the middle of a wrapped row is shown");
    if (count_on_screen(s, "cccccccccccccccccccc") != 1)
        fail("the tail of a wrapped row is shown, not clipped");
}

int main(void)
{
    set_size(80, 24);

    char path[] = "/tmp/mux-viewporttest-XXXXXX";
    int  wfd = mkstemp(path);
    tap_read = wfd >= 0 ? open(path, O_RDONLY) : -1;
    if (wfd < 0 || tap_read < 0) {
        fprintf(stderr, "viewporttest: no temp file\n");
        return 1;
    }
    unlink(path);
    fflush(stdout);
    if (dup2(wfd, STDOUT_FILENO) < 0) {
        fprintf(stderr, "viewporttest: cannot redirect stdout\n");
        return 1;
    }
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);

    ui_init();
    viewport_begin();

    struct screen s;
    screen_init(&s, 24, 80);

    check_tail(&s);
    check_bottom_up(&s);
    check_scroll(&s);
    check_soft_wrap(&s);
    check_reflow(&s);
    check_resize_strands_nothing(&s);

    fflush(stdout);
    if (failures)
        return 1;
    fprintf(stderr, "viewporttest: all checks passed\n");
    return 0;
}
