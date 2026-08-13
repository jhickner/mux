/**
 * kitty.h - kitty graphics protocol emitter (single-header, stb-style)
 *
 * In exactly ONE .c file:
 *
 *     #define KITTY_IMPLEMENTATION
 *     #include "kitty.h"
 *
 * Images are transmitted once (kg_transmit) and then placed as many times as
 * needed. Placements are cheap; transmission is not. All commands carry q=2 so
 * the terminal stays silent - responses would otherwise land in term.h's input
 * queue and be decoded as keystrokes.
 *
 * Output goes through term.h's buffered writer, so call term_flush() to push a
 * frame out.
 *
 * TWO PLACEMENT STYLES
 *
 *   Unicode placeholders (kg_virtual_place + placeholder cells) are the default
 *   and the only style that works under tmux. The image is anchored to real
 *   text cells - U+10EEEE carrying the image id in its foreground color and
 *   row/column in combining diacritics - so a multiplexer or editor that knows
 *   nothing about graphics still moves, scrolls and redraws it correctly.
 *   Emitting the cells is the caller's job; screen.h does it via
 *   glyph_placeholder().
 *
 *   Direct placement (kg_place) puts the image at the cursor with optional
 *   sub-cell pixel offsets. Nothing tracks it, so tmux loses it on the first
 *   redraw. It buys pixel-exact positioning without cell-aligned padding.
 *
 *   Placeholders can match direct placement's precision: scale the image to
 *   exactly cols*cell_w by rows*cell_h and letterbox inside that buffer, which
 *   is what kg_fit_cells() computes.
 *
 * TMUX
 *
 *   Every graphics escape is an APC sequence tmux does not understand, so under
 *   tmux each one must be wrapped in a DCS passthrough with its ESCs doubled.
 *   kg_init() detects $TMUX and does this for you from then on. tmux also has
 *   to be told to allow it at all - see kg_tmux_allow_passthrough().
 *
 * IMAGE IDS
 *
 *   In placeholder mode the id travels in the cell's foreground color, so it
 *   must fit in 24 bits. Ids above that need a third diacritic carrying the
 *   high byte; kg_placeholder_cell() emits it, but staying under 2^24 keeps the
 *   encoding to two diacritics. Id 0 is not valid.
 */

#ifndef KITTY_H
#define KITTY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef TERM_H
#include "term.h"
#endif

// The placeholder base character. Every cell of a placeholder placement holds
// this codepoint; row/column arrive as combining marks.
#define KG_PLACEHOLDER_CP 0x10EEEEu

// Largest image id representable without a third diacritic.
#define KG_MAX_ID_24BIT 0xFFFFFFu

// Highest row or column a placeholder can address (the diacritic table runs
// out at 297 entries).
int kg_max_rowcolumn(void);

// Detect the environment once, before any drawing. Enables tmux passthrough
// wrapping if $TMUX is set. Safe to skip if you never run under tmux.
void kg_init(void);

// Force passthrough wrapping on or off, overriding what kg_init() detected.
void kg_set_passthrough(bool on);

// True if escapes are currently being wrapped for tmux.
bool kg_passthrough(void);

// Bracket a block of placeholder cells written straight to the terminal, so
// that tmux 3.7 and later put them on screen.
//
// Those releases drop a cell's combining diacritics whenever the pane is not at
// column 0: screen_write_combine() tests visibility with a pane-relative x
// against window coordinates, decides the cell is covered by whatever pane
// really sits there, and skips the write. A placeholder cell is nothing but
// combining marks, so the image ends up smeared into bands.
//
// The cells still reach tmux's grid intact - only the write to the terminal is
// skipped - so ending a synchronized update, which tmux consumes itself and
// answers with a full pane redraw out of that grid, puts them up correctly.
// Outside tmux, and on tmux before 3.7, both calls are no-ops.
//
// Caveat: tmux 3.7a alone does not redraw when the update ends, so there the
// cells wait for its one-second sync timer.
void kg_placeholder_redraw_begin(void);
void kg_placeholder_redraw_end(void);

// Close a graphics escape that was abandoned part-way through.
//
// A frame given up on mid-transmit - a quit, a failed write - leaves the
// terminal inside an unterminated APC, and everything written after it is eaten
// as more of that escape: the delete on the way out, the mode resets, the
// shell's own prompt. That is what a quit during a busy frame looks like, a
// shell that has lost its terminal.
//
// This one goes through term.h's buffer and flushes, so it lands after whatever
// partial escape is already sitting there. Callers writing to a fd themselves
// want kg_abort_fd().
void kg_abort(void);

// As above for callers not using term.h's buffered writer: writes straight to
// `fd`, retrying briefly around a full pipe. Deliberately not writeall() - the
// reason for aborting may be that the terminal is taking nothing, so it tries a
// few times and gives up rather than blocking. Returns true if it all went out.
bool kg_abort_fd(int fd);

// Ask tmux to permit DCS passthrough for the current pane, which it refuses by
// default. Returns true if the option was set (or no tmux is involved, so
// nothing was needed). Scoped to the pane, so it neither disturbs other panes
// nor outlives them; a global equivalent belongs in the user's tmux.conf:
//
//     set -gq allow-passthrough all
//
// Runs `tmux set -p allow-passthrough all` via the tmux binary, so it does
// nothing useful if tmux is not on PATH.
bool kg_tmux_allow_passthrough(void);

// Payload bytes packed into one tmux passthrough DCS while batching.
#define KG_WRAP_MAX 262144

// Share one tmux passthrough DCS between every escape emitted until the
// matching kg_batch_end(). Nests; outside tmux both are no-ops.
//
// tmux ends every passthrough DCS with tty_invalidate(), which re-sends a
// cursor-style reset, a mouse-mode reset and a cursor move to home - to
// whichever pane is active, not the one drawing. The cost is per DCS, not per
// byte, so a frame that wraps each of its hundred 4KB chunks separately makes
// the cursor in another pane stutter a hundred times. The transmit functions
// below already bracket their own chunk loops; call these yourself only to
// batch several of them together.
//
// The wrapper is closed and reopened every KG_WRAP_MAX bytes. tmux discards a
// DCS longer than its input-buffer-size silently and whole - the placement still
// arrives, so the image comes out blank rather than erroring - and that option
// cannot be set below its 1MB default.
void kg_batch_begin(void);
void kg_batch_end(void);

// Send RGB pixel data (w*h*3 bytes) to the terminal under `id`, without
// displaying it. Safe to call again with the same id to replace the data.
void kg_transmit(uint32_t id, const uint8_t *rgb, int w, int h);

// As above for 3 (RGB) or 4 (RGBA) channels. RGBA is what letterbox padding
// wants: transparent pixels let the terminal's own background show through, so
// fitting an image to a cell rectangle doesn't paint a box around it.
void kg_transmit_ex(uint32_t id, const uint8_t *px, int w, int h, int channels);

// As above, but hand the terminal a file of raw pixel data to read itself
// instead of base64 in the escape stream. The escape stays a few dozen bytes
// however large the image is, which is what makes redrawing a full-screen image
// at animation rates practical - especially under tmux, where every byte of a
// passthrough payload is copied twice.
//
// The terminal deletes the file once read, so this only accepts paths it will
// agree to delete: kitty requires them to sit in a temporary directory AND to
// contain "tty-graphics-protocol". kg_tempfile() builds a conforming path.
// Returns false if the file could not be written; the caller keeps ownership of
// the path either way and should unlink it if this fails.
//
// Only the local terminal can open the file, so this is useless over ssh unless
// the terminal is on the same host. kg_transmit_ex() is the portable fallback.
bool kg_transmit_path(uint32_t id, const char *path, int w, int h, int channels);

// Write `px` to a fresh temporary file whose name satisfies kg_transmit_path()
// and store it in `out` (at least `cap` bytes; PATH_MAX is always enough).
// Returns false and leaves no file behind on failure.
bool kg_tempfile(char *out, size_t cap, const uint8_t *px, size_t bytes);

// Convenience: kg_tempfile() then kg_transmit_path(), flushing so the escape is
// on its way. Returns false if the data never went out, in which case no file is
// left behind.
//
// On success the file is the terminal's to delete, and there is no way to
// confirm it did: unlinking here would race the terminal's own read. A terminal
// that accepts t=t and ignores the deletion leaks one file per call, so callers
// looping on this should prefer a single reused id over unbounded new ones.
bool kg_transmit_via_file(uint32_t id, const uint8_t *px, int w, int h, int channels);

// Send an already-encoded PNG (f=100). The terminal reads the dimensions out of
// the PNG itself, so none are passed. Use this when the image arrives encoded -
// a screenshot off the wire, a file read whole - rather than decoding it only to
// have kg_transmit_ex() re-encode the pixels as base64.
void kg_transmit_png(uint32_t id, const uint8_t *png, size_t bytes);

// As above when the caller already holds the base64 rather than the PNG bytes,
// which saves copying a quarter-megabyte frame through an encoder that has
// nothing to do. `b64` must be the standard alphabet with no newlines.
void kg_transmit_png_b64(uint32_t id, const char *b64, size_t len);

// Declare that image `id` will be drawn into a cols x rows cell rectangle by
// placeholder cells appearing later. Draws nothing by itself. The image is
// fitted to the rectangle preserving aspect ratio, so pass a buffer already
// sized to the rectangle if you want no scaling of your own to be undone.
void kg_virtual_place(uint32_t id, int cols, int rows);

// As above, naming the placement `pid` so it can later be re-asserted or removed
// by kg_clear_placement(). Worth doing if the image data is ever replaced:
// re-transmitting under an existing id can drop its placements, and re-sending
// one 40-byte escape per frame is cheaper than discovering which terminals do.
void kg_virtual_place_p(uint32_t id, uint32_t pid, int cols, int rows);

// Emit one placeholder cell for image `id` at image-relative (row, col),
// including the SGR foreground that carries the id. Ordinary text - no
// passthrough involved. Callers driving a cell buffer usually want screen.h's
// glyph_placeholder() instead, which batches these properly.
void kg_placeholder_cell(uint32_t id, int row, int col);

// Emit a whole cols x rows block of placeholder cells with its top-left at
// screen cell (col, row), 0-based, restoring the default foreground after.
//
// Prefer this to a kg_placeholder_cell() loop when writing directly to the
// terminal: that one re-emits the 19-byte truecolor SGR for every cell, so a
// 100x40 placement spends 4000 snprintf calls and ~76KB on a colour that never
// changes. This sets it once and positions the cursor per row.
//
// Not for callers driving a cell buffer - screen.h's glyph_placeholder() puts
// the cells in the buffer where its own diffing can see them, which is what
// keeps a redraw from re-sending the whole block.
void kg_placeholder_rect(uint32_t id, int col, int row, int cols, int rows);

// Cell rectangle and pixel size an image should be scaled to in placeholder
// mode: the largest cell-aligned box within max_cols x max_rows that preserves
// the image's aspect ratio. Letterbox the scaled image inside *px_w x *px_h to
// center it, since placeholders have no sub-cell offset.
void kg_fit_cells(int img_w, int img_h, int cell_w, int cell_h,
                  int max_cols, int max_rows,
                  int *out_cols, int *out_rows, int *px_w, int *px_h);

// Display image `id` at cell (col, row), 0-based, using placement id `pid`.
// x_off/y_off nudge it by up to one cell in pixels, which is what makes
// pixel-exact centering inside a cell-aligned box possible. Direct placement:
// not tracked by tmux, so prefer placeholders there.
void kg_place(uint32_t id, uint32_t pid, int col, int row, int x_off, int y_off);

// Remove every placement on screen. Transmitted image data is kept, so the
// placements can be recreated without resending pixels. Placeholder placements
// disappear by overwriting their cells instead, which needs none of this.
void kg_clear_placements(void);

// Remove one placement. Not every terminal honours the blanket clear above, so
// callers that must be certain an image is gone name it explicitly. The image
// data is kept.
void kg_clear_placement(uint32_t id, uint32_t pid);

// Free image `id` and any of its placements.
void kg_delete(uint32_t id);

// Free every image this process transmitted.
void kg_delete_all(void);

// True if the terminal is likely to understand the protocol, judging only by
// the environment. Unreliable under tmux, where TERM describes tmux and the
// outer terminal is visible only through variables it happened to export - a
// session reattached from a different terminal will be misjudged. Prefer
// kg_probe() when the answer matters.
bool kg_supported(void);

// Ask the terminal itself, before any TUI setup: transmit a 1x1 image with a
// response requested, followed by a primary device attributes request. A
// terminal that speaks the protocol answers the first; one that doesn't still
// answers the second, which is what lets a negative answer be distinguished
// from a slow one. Puts stdin in raw mode for the duration and restores it.
//
// Call kg_init() first so the query is wrapped for tmux when needed.
//
// Under tmux the device attributes trick is skipped, because tmux answers that
// one itself, locally and immediately, while the graphics query still has to
// reach the outer terminal and come back - a race the local reply always wins,
// which would report every tmux pane as unsupported. So under tmux a terminal
// that stays silent yields -1 rather than 0, and the caller should fall back to
// kg_supported(). The cost is waiting out timeout_ms when there is no support.
//
// Returns 1 (supported), 0 (answered, but not supported), or -1 (no answer
// within timeout_ms, or stdin/stdout is not a terminal).
int kg_probe(int timeout_ms);

#endif // KITTY_H

/* ======================================================================== */
/* Implementation                                                           */
/* ======================================================================== */
#ifdef KITTY_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/time.h>

// Combining marks used to encode row/column numbers, from kitty's
// gen/rowcolumn-diacritics.txt: the Unicode 6.0.0 combining class 230 marks
// that have no decomposition mapping, minus those that could be fused into a
// precomposed character by normalization. Index N encodes row/column N.
static const uint32_t kg_diacritics[] = {
    0x0305,0x030D,0x030E,0x0310,0x0312,0x033D,0x033E,0x033F,
    0x0346,0x034A,0x034B,0x034C,0x0350,0x0351,0x0352,0x0357,
    0x035B,0x0363,0x0364,0x0365,0x0366,0x0367,0x0368,0x0369,
    0x036A,0x036B,0x036C,0x036D,0x036E,0x036F,0x0483,0x0484,
    0x0485,0x0486,0x0487,0x0592,0x0593,0x0594,0x0595,0x0597,
    0x0598,0x0599,0x059C,0x059D,0x059E,0x059F,0x05A0,0x05A1,
    0x05A8,0x05A9,0x05AB,0x05AC,0x05AF,0x05C4,0x0610,0x0611,
    0x0612,0x0613,0x0614,0x0615,0x0616,0x0617,0x0657,0x0658,
    0x0659,0x065A,0x065B,0x065D,0x065E,0x06D6,0x06D7,0x06D8,
    0x06D9,0x06DA,0x06DB,0x06DC,0x06DF,0x06E0,0x06E1,0x06E2,
    0x06E4,0x06E7,0x06E8,0x06EB,0x06EC,0x0730,0x0732,0x0733,
    0x0735,0x0736,0x073A,0x073D,0x073F,0x0740,0x0741,0x0743,
    0x0745,0x0747,0x0749,0x074A,0x07EB,0x07EC,0x07ED,0x07EE,
    0x07EF,0x07F0,0x07F1,0x07F3,0x0816,0x0817,0x0818,0x0819,
    0x081B,0x081C,0x081D,0x081E,0x081F,0x0820,0x0821,0x0822,
    0x0823,0x0825,0x0826,0x0827,0x0829,0x082A,0x082B,0x082C,
    0x082D,0x0951,0x0953,0x0954,0x0F82,0x0F83,0x0F86,0x0F87,
    0x135D,0x135E,0x135F,0x17DD,0x193A,0x1A17,0x1A75,0x1A76,
    0x1A77,0x1A78,0x1A79,0x1A7A,0x1A7B,0x1A7C,0x1B6B,0x1B6D,
    0x1B6E,0x1B6F,0x1B70,0x1B71,0x1B72,0x1B73,0x1CD0,0x1CD1,
    0x1CD2,0x1CDA,0x1CDB,0x1CE0,0x1DC0,0x1DC1,0x1DC3,0x1DC4,
    0x1DC5,0x1DC6,0x1DC7,0x1DC8,0x1DC9,0x1DCB,0x1DCC,0x1DD1,
    0x1DD2,0x1DD3,0x1DD4,0x1DD5,0x1DD6,0x1DD7,0x1DD8,0x1DD9,
    0x1DDA,0x1DDB,0x1DDC,0x1DDD,0x1DDE,0x1DDF,0x1DE0,0x1DE1,
    0x1DE2,0x1DE3,0x1DE4,0x1DE5,0x1DE6,0x1DFE,0x20D0,0x20D1,
    0x20D4,0x20D5,0x20D6,0x20D7,0x20DB,0x20DC,0x20E1,0x20E7,
    0x20E9,0x20F0,0x2CEF,0x2CF0,0x2CF1,0x2DE0,0x2DE1,0x2DE2,
    0x2DE3,0x2DE4,0x2DE5,0x2DE6,0x2DE7,0x2DE8,0x2DE9,0x2DEA,
    0x2DEB,0x2DEC,0x2DED,0x2DEE,0x2DEF,0x2DF0,0x2DF1,0x2DF2,
    0x2DF3,0x2DF4,0x2DF5,0x2DF6,0x2DF7,0x2DF8,0x2DF9,0x2DFA,
    0x2DFB,0x2DFC,0x2DFD,0x2DFE,0x2DFF,0xA66F,0xA67C,0xA67D,
    0xA6F0,0xA6F1,0xA8E0,0xA8E1,0xA8E2,0xA8E3,0xA8E4,0xA8E5,
    0xA8E6,0xA8E7,0xA8E8,0xA8E9,0xA8EA,0xA8EB,0xA8EC,0xA8ED,
    0xA8EE,0xA8EF,0xA8F0,0xA8F1,0xAAB0,0xAAB2,0xAAB3,0xAAB7,
    0xAAB8,0xAABE,0xAABF,0xAAC1,0xFE20,0xFE21,0xFE22,0xFE23,
    0xFE24,0xFE25,0xFE26,0x10A0F,0x10A38,0x1D185,0x1D186,0x1D187,
    0x1D188,0x1D189,0x1D1AA,0x1D1AB,0x1D1AC,0x1D1AD,0x1D242,0x1D243,
    0x1D244,
};

#define KG_DIACRITIC_COUNT ((int)(sizeof kg_diacritics / sizeof kg_diacritics[0]))

int kg_max_rowcolumn(void) { return KG_DIACRITIC_COUNT - 1; }

static bool kg_wrap = false;
static bool kg_redraw = false;

void kg_set_passthrough(bool on) { kg_wrap = on; }
bool kg_passthrough(void) { return kg_wrap; }

// "tmux 3.7b" / "tmux next-3.8" -> 307 / 308. 0 when it cannot be parsed.
static int kg_tmux_version(void) {
    FILE *p = popen("tmux -V 2>/dev/null", "r");
    if (!p) return 0;
    char buf[64] = "";
    bool got = fgets(buf, sizeof buf, p) != NULL;
    pclose(p);
    if (!got) return 0;
    const char *s = buf;
    while (*s && (*s < '0' || *s > '9')) s++;
    int maj = 0, min = 0;
    if (sscanf(s, "%d.%d", &maj, &min) < 2) return 0;
    return maj * 100 + min;
}

void kg_init(void) {
    kg_wrap = getenv("TMUX") != NULL;
    kg_redraw = kg_wrap && kg_tmux_version() >= 307;
}

// Unwrapped on purpose: these are addressed to tmux, not to the terminal.
void kg_placeholder_redraw_begin(void) {
    if (kg_redraw) term_write_n("\x1b[?2026h", 8);
}

void kg_placeholder_redraw_end(void) {
    if (kg_redraw) term_write_n("\x1b[?2026l", 8);
}

bool kg_tmux_allow_passthrough(void) {
    if (!getenv("TMUX")) return true;
    // >/dev/null so tmux's own output never reaches the terminal we are drawing
    // on. system() is acceptable here: no untrusted data is interpolated.
    return system("tmux set -p allow-passthrough all >/dev/null 2>&1") == 0;
}

static int kg_batch_depth = 0;      // nested kg_batch_begin() calls
static bool kg_batch_open = false;  // a passthrough DCS is open, unterminated
static size_t kg_batch_len = 0;     // payload bytes written into it

// The DCS payload with its ESCs doubled, which is what tmux forwards.
static void kg_emit_escaped(const char *seq, int n) {
    // Copy runs between ESCs rather than testing every byte: the payload is
    // mostly base64, which cannot contain one, so a quarter-megabyte frame is a
    // handful of memchr calls instead of a quarter-million comparisons.
    const char *end = seq + n;
    while (seq < end) {
        const char *e = memchr(seq, '\x1b', (size_t)(end - seq));
        if (!e) break;
        term_write_n(seq, (int)(e - seq) + 1);
        term_write_n("\x1b", 1);           // tmux eats one ESC of each pair
        seq = e + 1;
    }
    term_write_n(seq, (int)(end - seq));
}

// One graphics escape, wrapped for tmux when needed. The payload always arrives
// complete (never split across calls) because the DCS wrapper has to enclose
// the whole APC sequence, ESC ... ST included. Inside a batch the wrapper is
// shared with the escapes either side of it.
static void kg_emit(const char *seq, int n) {
    if (!kg_wrap) {
        term_write_n(seq, n);
        return;
    }
    if (kg_batch_depth) {
        if (kg_batch_open && kg_batch_len + (size_t)n > KG_WRAP_MAX) {
            term_write("\x1b\\");
            kg_batch_open = false;
        }
        if (!kg_batch_open) {
            term_write("\x1bPtmux;");
            kg_batch_open = true;
            kg_batch_len = 0;
        }
        kg_emit_escaped(seq, n);
        kg_batch_len += (size_t)n;
        return;
    }
    term_write("\x1bPtmux;");
    kg_emit_escaped(seq, n);
    term_write("\x1b\\");
}

// Closing here rather than on the next kg_emit() keeps the invariant that no
// unterminated DCS outlives the call that opened it: tmux resets one only after
// 5s, so anything written meanwhile would be swallowed.
static void kg_batch_close(void) {
    if (kg_batch_open) term_write("\x1b\\");
    kg_batch_open = false;
    kg_batch_len = 0;
}

void kg_batch_begin(void) { kg_batch_depth++; }

void kg_batch_end(void) {
    if (kg_batch_depth > 0 && --kg_batch_depth > 0) return;
    kg_batch_depth = 0;
    kg_batch_close();
}

// Under tmux the abandoned payload sits inside a passthrough DCS, so the inner
// ST goes in with its ESC doubled and the DCS is closed after it: ESC ESC \ ESC
// \, five bytes. Without tmux it is the bare ST.
#define KG_ABORT_SEQ_TMUX "\x1b\x1b\\\x1b\\"
#define KG_ABORT_SEQ      "\x1b\\"

// Both of these close the passthrough DCS outright, so an abandoned batch ends
// with them rather than leaking its wrapper into whatever is written next.
static void kg_batch_forget(void) {
    kg_batch_depth = 0;
    kg_batch_open = false;
    kg_batch_len = 0;
}

void kg_abort(void) {
    term_write(kg_wrap ? KG_ABORT_SEQ_TMUX : KG_ABORT_SEQ);
    term_flush();
    kg_batch_forget();
}

bool kg_abort_fd(int fd) {
    const char *seq = kg_wrap ? KG_ABORT_SEQ_TMUX : KG_ABORT_SEQ;
    size_t n = kg_wrap ? sizeof KG_ABORT_SEQ_TMUX - 1 : sizeof KG_ABORT_SEQ - 1;
    kg_batch_forget();
    for (int try = 0; try < 3 && n; try++) {
        ssize_t w = write(fd, seq, n);
        if (w == (ssize_t)n) return true;
        if (w > 0) { seq += w; n -= (size_t)w; }
        struct pollfd pf = { fd, POLLOUT, 0 };
        poll(&pf, 1, 50);
    }
    return false;
}

// Emit a string literal, letting the compiler supply the length.
#define kg_emit_lit(s) kg_emit((s), (int)(sizeof(s) - 1))

static const char KG_B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Encode `n` bytes (n <= 3) into 4 base64 chars with padding.
static void kg_b64_group(const uint8_t *src, int n, char *dst) {
    uint32_t v = (uint32_t)src[0] << 16;
    if (n > 1) v |= (uint32_t)src[1] << 8;
    if (n > 2) v |= (uint32_t)src[2];
    dst[0] = KG_B64[(v >> 18) & 0x3F];
    dst[1] = KG_B64[(v >> 12) & 0x3F];
    dst[2] = n > 1 ? KG_B64[(v >> 6) & 0x3F] : '=';
    dst[3] = n > 2 ? KG_B64[v & 0x3F] : '=';
}

#define KG_CHUNK 4096   // base64 chars per escape sequence (protocol maximum)

void kg_transmit(uint32_t id, const uint8_t *rgb, int w, int h) {
    kg_transmit_ex(id, rgb, w, h, 3);
}

void kg_transmit_ex(uint32_t id, const uint8_t *rgb, int w, int h, int channels) {
    if (!rgb || w <= 0 || h <= 0 || (channels != 3 && channels != 4)) return;

    size_t total = (size_t)w * (size_t)h * (size_t)channels;
    size_t bytes_per_chunk = (KG_CHUNK / 4) * 3;
    // Header, payload and terminator go out as one buffer so kg_emit() can wrap
    // the sequence as a unit.
    char buf[128 + KG_CHUNK + 2];
    bool first = true;

    kg_batch_begin();
    for (size_t off = 0; off < total; off += bytes_per_chunk) {
        size_t n = total - off;
        if (n > bytes_per_chunk) n = bytes_per_chunk;
        bool last = (off + n >= total);

        int len;
        if (first) {
            len = snprintf(buf, sizeof buf,
                           "\x1b_Ga=t,f=%d,s=%d,v=%d,i=%u,q=2,m=%d;",
                           channels == 4 ? 32 : 24, w, h, id, last ? 0 : 1);
            first = false;
        } else {
            len = snprintf(buf, sizeof buf, "\x1b_Gm=%d,q=2;", last ? 0 : 1);
        }

        for (size_t i = 0; i < n; i += 3) {
            int g = (int)(n - i);
            if (g > 3) g = 3;
            kg_b64_group(rgb + off + i, g, buf + len);
            len += 4;
        }
        buf[len++] = '\x1b';
        buf[len++] = '\\';
        kg_emit(buf, len);
    }
    kg_batch_end();
}

// f=100 says the payload is a PNG rather than raw pixels, so s= and v= are
// omitted: the terminal takes the dimensions from the PNG header.
#define KG_PNG_HDR "\x1b_Ga=t,f=100,t=d,i=%u,q=2,m=%d;"

void kg_transmit_png(uint32_t id, const uint8_t *png, size_t bytes) {
    if (!png || !bytes) return;

    // Divisible by 3, so every chunk but the last encodes without padding and
    // the decoder sees one continuous stream rather than a run of terminated
    // base64 fragments.
    size_t per_chunk = (KG_CHUNK / 4) * 3;
    char buf[128 + KG_CHUNK + 2];
    bool first = true;

    kg_batch_begin();
    for (size_t off = 0; off < bytes; off += per_chunk) {
        size_t n = bytes - off;
        if (n > per_chunk) n = per_chunk;
        bool last = (off + n >= bytes);

        int len = first ? snprintf(buf, sizeof buf, KG_PNG_HDR, id, last ? 0 : 1)
                        : snprintf(buf, sizeof buf, "\x1b_Gm=%d,q=2;", last ? 0 : 1);
        first = false;

        for (size_t i = 0; i < n; i += 3) {
            int g = (int)(n - i);
            if (g > 3) g = 3;
            kg_b64_group(png + off + i, g, buf + len);
            len += 4;
        }
        buf[len++] = '\x1b';
        buf[len++] = '\\';
        kg_emit(buf, len);
    }
    kg_batch_end();
}

void kg_transmit_png_b64(uint32_t id, const char *b64, size_t len) {
    if (!b64 || !len) return;

    char buf[128 + KG_CHUNK + 2];
    bool first = true;

    // KG_CHUNK is a multiple of 4, so a chunk boundary never falls inside a
    // base64 quad.
    kg_batch_begin();
    for (size_t off = 0; off < len; off += KG_CHUNK) {
        size_t n = len - off;
        if (n > KG_CHUNK) n = KG_CHUNK;
        bool last = (off + n >= len);

        int hn = first ? snprintf(buf, sizeof buf, KG_PNG_HDR, id, last ? 0 : 1)
                       : snprintf(buf, sizeof buf, "\x1b_Gm=%d,q=2;", last ? 0 : 1);
        first = false;

        memcpy(buf + hn, b64 + off, n);
        hn += (int)n;
        buf[hn++] = '\x1b';
        buf[hn++] = '\\';
        kg_emit(buf, hn);
    }
    kg_batch_end();
}

// Base64 of a path is tiny, so unlike pixel payloads it never needs chunking.
static int kg_b64_str(const char *src, size_t n, char *dst, size_t cap) {
    if ((n + 2) / 3 * 4 + 1 > cap) return -1;
    int len = 0;
    for (size_t i = 0; i < n; i += 3) {
        int g = (int)(n - i);
        if (g > 3) g = 3;
        kg_b64_group((const uint8_t *)src + i, g, dst + len);
        len += 4;
    }
    dst[len] = '\0';
    return len;
}

bool kg_transmit_path(uint32_t id, const char *path, int w, int h, int channels) {
    if (!path || w <= 0 || h <= 0 || (channels != 3 && channels != 4)) return false;

    char b64[1024];
    if (kg_b64_str(path, strlen(path), b64, sizeof b64) < 0) return false;

    char cmd[1280];
    // t=t: the payload names a temporary file, which the terminal reads and then
    // deletes. f=24/32 still describes the pixel format inside it.
    int n = snprintf(cmd, sizeof cmd, "\x1b_Ga=t,t=t,f=%d,s=%d,v=%d,i=%u,q=2;%s\x1b\\",
                     channels == 4 ? 32 : 24, w, h, id, b64);
    if (n <= 0 || n >= (int)sizeof cmd) return false;
    kg_emit(cmd, n);
    return true;
}

bool kg_tempfile(char *out, size_t cap, const uint8_t *px, size_t bytes) {
    if (!out || !px) return false;

    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";

    size_t dlen = strlen(dir);
    // The name has to carry this marker or kitty refuses to delete the file.
    int n = snprintf(out, cap, "%s%stty-graphics-protocol-XXXXXX",
                     dir, (dlen && dir[dlen - 1] == '/') ? "" : "/");
    if (n <= 0 || n >= (int)cap) return false;

    int fd = mkstemp(out);
    if (fd < 0) return false;

    bool ok = true;
    for (size_t off = 0; off < bytes; ) {
        ssize_t w = write(fd, px + off, bytes - off);
        if (w <= 0) { ok = false; break; }
        off += (size_t)w;
    }
    close(fd);
    if (!ok) unlink(out);
    return ok;
}

bool kg_transmit_via_file(uint32_t id, const uint8_t *px, int w, int h, int channels) {
    if (!px || w <= 0 || h <= 0 || (channels != 3 && channels != 4)) return false;

    char path[4096];
    size_t bytes = (size_t)w * (size_t)h * (size_t)channels;
    if (!kg_tempfile(path, sizeof path, px, bytes)) return false;

    if (!kg_transmit_path(id, path, w, h, channels)) {
        unlink(path);
        return false;
    }
    // The terminal reads the file when it processes the escape, which has not
    // necessarily happened yet, so this cannot unlink now. Flushing hands the
    // escape over; the file is the terminal's to remove from here.
    term_flush();
    return true;
}

void kg_virtual_place(uint32_t id, int cols, int rows) {
    if (cols <= 0 || rows <= 0) return;
    char cmd[128];
    // U=1 marks the placement virtual: it reserves nothing on screen and draws
    // nothing until placeholder cells referring to this id appear.
    int n = snprintf(cmd, sizeof cmd, "\x1b_Ga=p,U=1,i=%u,c=%d,r=%d,q=2\x1b\\",
                     id, cols, rows);
    kg_emit(cmd, n);
}

void kg_virtual_place_p(uint32_t id, uint32_t pid, int cols, int rows) {
    if (cols <= 0 || rows <= 0) return;
    char cmd[128];
    int n = snprintf(cmd, sizeof cmd,
                     "\x1b_Ga=p,U=1,i=%u,p=%u,c=%d,r=%d,q=2\x1b\\",
                     id, pid, cols, rows);
    kg_emit(cmd, n);
}

static int kg_utf8(uint32_t cp, char *out) {
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

void kg_placeholder_cell(uint32_t id, int row, int col) {
    if (row < 0 || col < 0 ||
        row >= KG_DIACRITIC_COUNT || col >= KG_DIACRITIC_COUNT) return;

    char buf[64];
    int n = snprintf(buf, sizeof buf, "\x1b[38;2;%u;%u;%um",
                     (id >> 16) & 0xFF, (id >> 8) & 0xFF, id & 0xFF);
    n += kg_utf8(KG_PLACEHOLDER_CP, buf + n);
    n += kg_utf8(kg_diacritics[row], buf + n);
    n += kg_utf8(kg_diacritics[col], buf + n);

    uint32_t high = id >> 24;
    if (high) n += kg_utf8(kg_diacritics[high], buf + n);

    term_write_n(buf, n);
}

void kg_placeholder_rect(uint32_t id, int col, int row, int cols, int rows) {
    if (col < 0 || row < 0 || cols <= 0 || rows <= 0) return;
    if (cols > KG_DIACRITIC_COUNT) cols = KG_DIACRITIC_COUNT;
    if (rows > KG_DIACRITIC_COUNT) rows = KG_DIACRITIC_COUNT;

    char cell[4];
    int celln = kg_utf8(KG_PLACEHOLDER_CP, cell);

    // The id's high byte rides in a third diacritic, identical on every cell of
    // the block, so it is encoded once here rather than per cell.
    char high[4];
    int highn = 0;
    uint32_t hi = id >> 24;
    if (hi) highn = kg_utf8(kg_diacritics[hi], high);

    char sgr[32];
    term_write_n(sgr, snprintf(sgr, sizeof sgr, "\x1b[38;2;%u;%u;%um",
                               (id >> 16) & 0xFF, (id >> 8) & 0xFF, id & 0xFF));

    // Largest single addition below is a cursor move (~16B) or a cell (16B), so
    // 64 bytes of headroom is enough to append without re-checking mid-cell.
    char buf[4096];
    const int flush_at = (int)sizeof buf - 64;
    int n = 0;

    for (int r = 0; r < rows; r++) {
        if (n > flush_at) { term_write_n(buf, n); n = 0; }
        n += snprintf(buf + n, sizeof buf - (size_t)n, "\x1b[%d;%dH",
                      row + r + 1, col + 1);
        for (int c = 0; c < cols; c++) {
            if (n > flush_at) { term_write_n(buf, n); n = 0; }
            memcpy(buf + n, cell, (size_t)celln);
            n += celln;
            n += kg_utf8(kg_diacritics[r], buf + n);
            n += kg_utf8(kg_diacritics[c], buf + n);
            if (highn) { memcpy(buf + n, high, (size_t)highn); n += highn; }
        }
    }
    if (n) term_write_n(buf, n);
    term_write("\x1b[39m");
}

void kg_fit_cells(int img_w, int img_h, int cell_w, int cell_h,
                  int max_cols, int max_rows,
                  int *out_cols, int *out_rows, int *px_w, int *px_h) {
    int cols = max_cols, rows = max_rows;

    if (img_w > 0 && img_h > 0 && cell_w > 0 && cell_h > 0 &&
        max_cols > 0 && max_rows > 0) {
        // Compare aspect ratios in pixels, then round the limiting dimension up
        // so the image never overflows the box it was told to fit.
        long box_w = (long)max_cols * cell_w, box_h = (long)max_rows * cell_h;
        if ((long)img_w * box_h > (long)img_h * box_w) {
            long want_h = (long)img_h * box_w / img_w;          // width-limited
            rows = (int)((want_h + cell_h - 1) / cell_h);
        } else {
            long want_w = (long)img_w * box_h / img_h;          // height-limited
            cols = (int)((want_w + cell_w - 1) / cell_w);
        }
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;
        if (cols > max_cols) cols = max_cols;
        if (rows > max_rows) rows = max_rows;
    }

    if (cols > KG_DIACRITIC_COUNT) cols = KG_DIACRITIC_COUNT;
    if (rows > KG_DIACRITIC_COUNT) rows = KG_DIACRITIC_COUNT;

    if (out_cols) *out_cols = cols;
    if (out_rows) *out_rows = rows;
    if (px_w) *px_w = cols * cell_w;
    if (px_h) *px_h = rows * cell_h;
}

void kg_place(uint32_t id, uint32_t pid, int col, int row, int x_off, int y_off) {
    char cmd[128];
    term_move_cursor(col, row);
    // C=1 keeps the cursor where it is, so a placement never scrolls the screen.
    int n = snprintf(cmd, sizeof cmd,
                     "\x1b_Ga=p,i=%u,p=%u,X=%d,Y=%d,C=1,z=0,q=2\x1b\\",
                     id, pid, x_off, y_off);
    kg_emit(cmd, n);
}

void kg_clear_placements(void) {
    // Lowercase 'a' deletes placements but keeps the pixel data cached.
    kg_emit_lit("\x1b_Ga=d,d=a,q=2\x1b\\");
}

void kg_clear_placement(uint32_t id, uint32_t pid) {
    char cmd[64];
    int n = snprintf(cmd, sizeof cmd, "\x1b_Ga=d,d=i,i=%u,p=%u,q=2\x1b\\", id, pid);
    kg_emit(cmd, n);
}

void kg_delete(uint32_t id) {
    char cmd[64];
    int n = snprintf(cmd, sizeof cmd, "\x1b_Ga=d,d=I,i=%u,q=2\x1b\\", id);
    kg_emit(cmd, n);
}

void kg_delete_all(void) {
    kg_emit_lit("\x1b_Ga=d,d=A,q=2\x1b\\");
}

bool kg_supported(void) {
    const char *v;
    if ((v = getenv("TERM")) && (strstr(v, "kitty") || strstr(v, "ghostty")))
        return true;
    if ((v = getenv("TERM_PROGRAM")) &&
        (strstr(v, "ghostty") || strstr(v, "Ghostty") || strstr(v, "WezTerm")))
        return true;
    if (getenv("KITTY_WINDOW_ID") || getenv("GHOSTTY_RESOURCES_DIR"))
        return true;
    return false;
}

#define KG_PROBE_GRAPHICS "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\"

static bool kg_write_all(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) return false;
        s += w; n -= (size_t)w;
    }
    return true;
}

static int kg_elapsed_ms(const struct timeval *start) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (int)((now.tv_sec - start->tv_sec) * 1000 +
                 (now.tv_usec - start->tv_usec) / 1000);
}

int kg_probe(int timeout_ms) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return -1;

    struct termios orig;
    if (tcgetattr(STDIN_FILENO, &orig) == -1) return -1;
    struct termios raw = orig;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return -1;

    // Built here rather than as a literal because the graphics half needs tmux
    // wrapping, and because the device attributes request is only useful when
    // tmux is not in the way to answer it locally.
    char query[256];
    int qn = 0;
    if (kg_wrap) {
        qn += snprintf(query + qn, sizeof query - qn, "\x1bPtmux;");
        for (const char *p = KG_PROBE_GRAPHICS; *p; p++) {
            if (*p == '\x1b') query[qn++] = '\x1b';
            query[qn++] = *p;
        }
        qn += snprintf(query + qn, sizeof query - qn, "\x1b\\");
    } else {
        qn += snprintf(query + qn, sizeof query - qn, "%s%s",
                       KG_PROBE_GRAPHICS, "\x1b[c");
    }

    int result = -1;
    if (kg_write_all(STDOUT_FILENO, query, (size_t)qn)) {
        char buf[512];
        size_t len = 0;
        struct timeval start;
        gettimeofday(&start, NULL);

        while (result == -1) {
            int left = timeout_ms - kg_elapsed_ms(&start);
            if (left <= 0) break;

            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            struct timeval tv = { left / 1000, (left % 1000) * 1000 };
            int r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
            if (r <= 0) break;

            ssize_t n = read(STDIN_FILENO, buf + len, sizeof buf - 1 - len);
            if (n <= 0) break;
            len += (size_t)n;
            buf[len] = '\0';

            char *da;
            if (strstr(buf, "_Gi=31;OK")) result = 1;
            // The device attributes reply (CSI ? … c) is answered by every
            // terminal and comes after the graphics response would have, so
            // seeing it complete means there wasn't one. Only true without tmux
            // in between - see the note on kg_probe().
            else if (!kg_wrap && (da = strstr(buf, "\x1b[?")) != NULL &&
                     memchr(da, 'c', len - (size_t)(da - buf))) result = 0;
            else if (len == sizeof buf - 1) break;
        }
    }

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
    return result;
}

#endif // KITTY_IMPLEMENTATION
