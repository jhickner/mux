// Nothing the markdown renderer draws may be wider than the width it was
// asked for. A row a cell or two over is not a rendering detail: the caller
// puts a gutter down the side of what comes back, and the screen then breaks
// the overflow wherever it likes — mid-word, with no gutter on what is left.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md.h"
#include "ui.h"

static int failures;

static void fail(const char *what, int width, size_t cells, const char *row, size_t len)
{
    fprintf(stderr, "FAIL %s: %d wide, row of %zu cells: [%.*s]\n", what, width, cells,
            (int)len, row);
    failures++;
}

// The widest row of `src` rendered at `width`, in cells.
static void check_fits(const char *what, const char *src, int width)
{
    ui_capture_begin(width);
    md_render(src, 0);
    char *out = ui_capture_end();
    if (!out)
        return;
    for (char *p = out; *p;) {
        char  *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        size_t cells = ui_cells_visible(p, len);
        if (cells > (size_t)width)
            fail(what, width, cells, p, len);
        if (!nl)
            break;
        p = nl + 1;
    }
    free(out);
}

// Every width in a range, because an overflow only shows when a word happens
// to land on the edge — one width proves nothing.
static void check_widths(const char *what, const char *src)
{
    for (int w = 20; w <= 120; w++)
        check_fits(what, src, w);
}

int main(void)
{
    setenv("COLUMNS", "80", 1);
    setenv("LINES", "24", 1);
    ui_init();

    check_widths("a paragraph",
                 "The generator already exists (tools/gen-testimages.sh) and has been run — "
                 "29 files are in testimages/ now, one per case.\n");

    // A bullet, a number and a quote bar all put something on the first row
    // before the text starts, and it is the first row that overflows.
    check_widths("a bulleted list",
                 "- PNG fast path (8): basic, wide, tall, strip-2000x40, strip-40x2000, "
                 "large-3000x2000, tiny-16, pixel-1x1\n"
                 "- Conversion path / non-PNG (8): photo.jpg, bmp, tiff, webp, ico, "
                 "anim.gif, doc.pdf, vector.svg\n");

    check_widths("a numbered list",
                 "1. Conversion path / non-PNG (8): photo.jpg, bmp, tiff, webp, ico, "
                 "anim.gif, doc.pdf, vector.svg\n"
                 "10. Failure paths (7): broken.png, empty.png, nonimage.png, noext, "
                 "truncated.jpg, oversize-20mb.png\n");

    check_widths("a quote",
                 "> Conversion path / non-PNG (8): photo.jpg, bmp, tiff, webp, ico, "
                 "anim.gif, doc.pdf, vector.svg\n");

    check_widths("a nested list",
                 "- Conversion path / non-PNG (8): photo.jpg, bmp, tiff, webp, ico\n"
                 "  - anim.gif, doc.pdf, vector.svg, and whatever else turns up here\n");

    // Styling and links take no cells, so they cannot make a row too wide.
    check_widths("styled text",
                 "- **Conversion path** / *non-PNG* (8): `photo.jpg`, [bmp](https://x.test/b), "
                 "tiff, webp, ico, anim.gif\n");

    if (failures)
        return 1;
    fprintf(stderr, "mdtest: all checks passed\n");
    return 0;
}
