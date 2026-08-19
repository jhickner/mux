// The viewport paints every cell of every row it owns, so what is on screen
// after a paint is fully determined by what mux wrote — no guess about how the
// terminal reflowed, and nothing left over from the paint before. This asserts
// that, including across the resizes the old block model could not survive.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "screenmodel.h"
#include "ui.h"
#include "viewport.h"

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

static int tap_read = -1;

// What a paint cost, in bytes actually sent to the terminal.
static size_t pumped;

static void pump(struct screen *s)
{
    fflush(stdout);
    char    buf[65536];
    ssize_t n;
    while ((n = read(tap_read, buf, sizeof buf)) > 0) {
        pumped += (size_t)n;
        feed(s, buf, (size_t)n);
    }
}



static void say(const char *text)
{
    viewport_write(text, strlen(text));
    viewport_write("\n", 1);
}

// A real terminal keeps what it was last sent, and the painter only sends what
// changed, so the model has to persist across paints the way a screen does.
static void redraw(struct screen *s)
{
    viewport_paint();
    pump(s);
}

// The screen is blank and nothing on it is ours: the next paint sends all of
// it. What a resize does, and what the test needs between cases.
static void refresh(struct screen *s, int cols, int rows)
{
    screen_init(s, rows, cols);
    viewport_forget();
    redraw(s);
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
        refresh(s, W[i], 24);

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

    refresh(s, 80, 24);

    // 24 rows, 1 of chrome: the chrome is row 24 and the three rows sit on
    // 21, 22, 23 rather than 1, 2, 3.
    if (!strstr(row_text(s, 23), "CHROME-prompt"))
        fail("the chrome is on the last row");
    if (!strstr(row_text(s, 22), "third"))
        fail("the newest row is directly above the chrome");
    if (!strstr(row_text(s, 20), "first"))
        fail("the oldest row is pushed down to meet it");
    for (int r = 0; r < 20; r++)
        if (strstr(row_text(s, r), "first") || strstr(row_text(s, r), "third"))
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

    refresh(s, 80, 24);

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

    refresh(s, 80, 24);
    viewport_scroll(10);
    pump(s);
    if (count_on_screen(s, "row 89") != 1)
        fail("scrolling up moves the window back by that many rows");
    if (count_on_screen(s, "row 99") != 0)
        fail("scrolling up drops the newest rows off the bottom");

    refresh(s, 80, 24);
    viewport_scroll_end();
    pump(s);
    if (count_on_screen(s, "row 99") != 1)
        fail("scrolling back to the end shows the newest row again");

    // Output arriving while scrolled up follows the tail again.
    viewport_scroll(10);
    pump(s);
    say("newest");
    redraw(s);
    if (count_on_screen(s, "newest") != 1)
        fail("new output returns the window to the tail");
}

// A screen full of long rows is what an image looks like from here: every row
// is kilobytes of placeholder cells. Scrolling has to move what the terminal
// already has rather than resend it, or a wheel tick costs the whole screen.
static void check_scroll_is_cheap(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);

    char line[400];
    memset(line, 'x', sizeof line - 1);
    line[sizeof line - 1] = '\0';
    for (int i = 0; i < 200; i++)
        say(line);
    chrome("CHROME-prompt", NULL);

    refresh(s, 80, 24);

    // A full repaint is the cost to beat.
    viewport_forget();
    pumped = 0;
    redraw(s);
    size_t full = pumped;
    if (full < 2000)
        fail("the screen under test is actually expensive to repaint");

    // A repaint with nothing changed sends only the frame's own preamble.
    pumped = 0;
    redraw(s);
    if (pumped > full / 20)
        fail("a paint with nothing changed sends next to nothing");

    pumped = 0;
    viewport_scroll(3);
    pump(s);
    size_t scrolled_cost = pumped;
    if (scrolled_cost >= full / 3)
        fail("scrolling costs a fraction of a full repaint");

    // ...and it still lands on the right rows.
    if (count_on_screen(s, "CHROME-prompt") != 1)
        fail("the chrome survives a scroll that moved rows");
}

// A kept entry can be changed after it is printed: the payload is looked up by
// mark, so a caller that holds the mark cannot be left pointing at an entry
// that is gone. This is what a live spinner on a /btw question rides on.
struct live {
    int  frame;
    int  done;
};

static void live_render(void *ud, int cols)
{
    (void)cols;
    const struct live *l = ud;
    ui_printf("LIVE-%s-%d", l->done ? "done" : "spin", l->frame);
    ui_put("\n");
}

static void check_live_entry(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);

    struct live *l = calloc(1, sizeof *l);
    unsigned mark = viewport_mark();
    viewport_item_begin(live_render, l, free);
    live_render(l, 80);
    viewport_item_end();

    refresh(s, 80, 24);
    if (count_on_screen(s, "LIVE-spin-0") != 1)
        fail("a live entry shows its first state");

    if (viewport_item_data(mark) != l)
        fail("the payload is reachable by mark");

    l->frame = 7;
    viewport_item_update(mark);
    refresh(s, 80, 24);
    if (count_on_screen(s, "LIVE-spin-7") != 1)
        fail("updating the payload redraws the entry");
    if (count_on_screen(s, "LIVE-spin-0") != 0)
        fail("the previous state of the entry is gone");

    // Output after it does not disturb it, and it stays put.
    say("after");
    l->done = 1;
    viewport_item_update(mark);
    refresh(s, 80, 24);
    if (count_on_screen(s, "LIVE-done-7") != 1)
        fail("an entry can still be changed once later output has landed");
    if (count_on_screen(s, "after") != 1)
        fail("the later output is still there");

    // Once the entry is gone the mark answers NULL rather than a stale pointer.
    viewport_clear();
    if (viewport_item_data(mark) != NULL)
        fail("a mark for a dropped entry reports nothing");
    viewport_item_update(mark);
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

// One begin/end pair is one entry, whatever was already in hand when it
// started. Anything that prints a block relies on this: an entry printed twice
// is a block of transcript repeated, which is what a duplicate reply looks
// like from the outside.
static void check_item_counting(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);

    unsigned before = viewport_mark();
    viewport_item_begin(width_render, NULL, NULL);
    width_render(NULL, 80);
    viewport_item_end();
    if (viewport_mark() - before != 1)
        fail("a begin/end pair makes exactly one entry");

    // Unwrapped output already in hand is closed into an entry of its own
    // rather than being swept into the one being opened.
    viewport_write("LOOSE\n", 6);
    viewport_write("PARTIAL", 7);
    before = viewport_mark();
    viewport_item_begin(width_render, NULL, NULL);
    width_render(NULL, 80);
    viewport_item_end();
    if (viewport_mark() - before != 2)
        fail("output in hand becomes an entry of its own");

    refresh(s, 80, 24);
    if (count_on_screen(s, "LOOSE") != 1)
        fail("a loose row is on screen once");
    if (count_on_screen(s, "PARTIAL") != 1)
        fail("a partial row is on screen once");
    if (count_on_screen(s, "WIDTH-80") != 2)
        fail("each entry is on screen once");
}

// An entry whose renderer captures on its own account. The viewport already
// re-renders inside a capture, so this nests one — a side turn's answer does
// exactly this, rendering its markdown at an inner width before putting a bar
// down every row it came back with.
static void nested_render(void *ud, int cols)
{
    (void)ud;
    ui_put("HEAD\n");

    ui_capture_begin(cols - 2);
    ui_printf("INNER-%d", ui_columns());
    ui_put("\n");
    ui_printf("INNER-%d", ui_columns());
    ui_put("\n");
    char *painted = ui_capture_end();

    // Every row of it gets a prefix, which is the part that goes missing when
    // the inner capture takes the outer one down with it.
    for (char *p = painted; p && *p;) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        ui_put("| ");
        ui_putn(p, len);
        ui_put("\n");
        if (!nl)
            break;
        p = nl + 1;
    }
    free(painted);
}

static void check_nested_capture(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);

    viewport_item_begin(nested_render, NULL, NULL);
    nested_render(NULL, 80);
    viewport_item_end();

    refresh(s, 80, 24);
    if (count_on_screen(s, "HEAD") != 1)
        fail("an entry with a nested capture keeps what it wrote first");
    if (count_on_screen(s, "| INNER-78") != 2)
        fail("every row of the inner render is prefixed");

    // The width change is what forces the re-render, and the re-render is what
    // runs the renderer inside a capture of the viewport's own.
    set_size(60, 24);
    refresh(s, 60, 24);
    if (count_on_screen(s, "HEAD") != 1)
        fail("a re-rendered entry still has what it wrote first");
    if (count_on_screen(s, "| INNER-58") != 2)
        fail("a re-rendered entry still prefixes every row");
    if (count_on_screen(s, "INNER-78") != 0)
        fail("nothing is left over from the width it was rendered at before");
}

static void check_reflow(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);

    rendered_at = 0;
    viewport_item_begin(width_render, NULL, NULL);
    width_render(NULL, 80);
    viewport_item_end();

    refresh(s, 80, 24);
    if (count_on_screen(s, "WIDTH-80") != 1)
        fail("a kept entry shows what it rendered at the first width");

    // Narrower: the entry is asked to render again rather than being chopped.
    set_size(48, 24);
    refresh(s, 48, 24);
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
    refresh(s, 48, 24);
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

    refresh(s, 20, 24);

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
    check_item_counting(&s);
    check_nested_capture(&s);
    check_reflow(&s);
    check_live_entry(&s);
    check_scroll_is_cheap(&s);
    check_resize_strands_nothing(&s);

    fflush(stdout);
    if (failures)
        return 1;
    fprintf(stderr, "viewporttest: all checks passed\n");
    return 0;
}
