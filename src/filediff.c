#include "filediff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ui.h"
#include "text.h"

#define MAX_BYTES  (1u << 20)
#define MAX_ROWS   16
#define CONTEXT    2
#define LCS_BUDGET 2000000L

static struct {
    int    have;
    char   path[4096];
    char  *before;
    size_t before_len;
} snap;

struct linevec {
    const char **p;
    size_t      *n;
    int          count;
};

static void lines_free(struct linevec *v)
{
    free(v->p);
    free(v->n);
    v->p = NULL;
    v->n = NULL;
    v->count = 0;
}

static int lines_split(struct linevec *v, const char *text, size_t len)
{
    v->p = NULL;
    v->n = NULL;
    v->count = 0;
    if (!text || len == 0)
        return 1;

    int count = 0;
    for (size_t i = 0; i < len; i++)
        if (text[i] == '\n')
            count++;
    if (text[len - 1] != '\n')
        count++;
    if (count == 0)
        return 1;

    v->p = calloc((size_t)count, sizeof *v->p);
    v->n = calloc((size_t)count, sizeof *v->n);
    if (!v->p || !v->n) {
        lines_free(v);
        return 0;
    }

    int i = 0;
    size_t start = 0;
    for (size_t k = 0; k < len && i < count; k++) {
        if (text[k] != '\n')
            continue;
        v->p[i] = text + start;
        v->n[i] = k - start;
        i++;
        start = k + 1;
    }
    if (i < count) {
        v->p[i] = text + start;
        v->n[i] = len - start;
        i++;
    }
    v->count = i;
    return 1;
}

static int line_eq(const struct linevec *a, int i, const struct linevec *b, int j)
{
    return a->n[i] == b->n[j] && memcmp(a->p[i], b->p[j], a->n[i]) == 0;
}

struct op {
    char        sign;
    const char *p;
    size_t      n;
};

static void op_push(struct op **ops, int *count, int *cap, char sign, const char *p, size_t n)
{
    if (*count == *cap) {
        int want = *cap ? *cap * 2 : 64;
        struct op *grown = realloc(*ops, (size_t)want * sizeof **ops);
        if (!grown)
            return;
        *ops = grown;
        *cap = want;
    }
    (*ops)[(*count)++] = (struct op){sign, p, n};
}

static void op_block(struct op **ops, int *count, int *cap, char sign, const struct linevec *v,
                     int from, int to)
{
    for (int i = from; i < to; i++)
        op_push(ops, count, cap, sign, v->p[i], v->n[i]);
}

static int lcs_script(struct op **ops, int *count, int *cap, const struct linevec *a,
                      const struct linevec *b, int pre, int ra, int rb)
{
    int *dp = calloc((size_t)(ra + 1) * (size_t)(rb + 1), sizeof *dp);
    if (!dp)
        return 0;

    int stride = rb + 1;
    for (int i = ra - 1; i >= 0; i--) {
        for (int j = rb - 1; j >= 0; j--) {
            if (line_eq(a, pre + i, b, pre + j)) {
                dp[i * stride + j] = dp[(i + 1) * stride + (j + 1)] + 1;
            } else {
                int drop_a = dp[(i + 1) * stride + j];
                int drop_b = dp[i * stride + (j + 1)];
                dp[i * stride + j] = drop_a > drop_b ? drop_a : drop_b;
            }
        }
    }

    int i = 0, j = 0;
    while (i < ra && j < rb) {
        if (line_eq(a, pre + i, b, pre + j)) {
            op_push(ops, count, cap, ' ', a->p[pre + i], a->n[pre + i]);
            i++;
            j++;
        } else if (dp[(i + 1) * stride + j] >= dp[i * stride + (j + 1)]) {
            op_push(ops, count, cap, '-', a->p[pre + i], a->n[pre + i]);
            i++;
        } else {
            op_push(ops, count, cap, '+', b->p[pre + j], b->n[pre + j]);
            j++;
        }
    }
    op_block(ops, count, cap, '-', a, pre + i, pre + ra);
    op_block(ops, count, cap, '+', b, pre + j, pre + rb);

    free(dp);
    return 1;
}

static void diff_text(const char *in, size_t n, char *out, size_t max)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < max; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\t') {
            for (int k = 0; k < 4 && o + 1 < max; k++)
                out[o++] = ' ';
        } else if (c >= 0x20 && c != 0x7f) {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static void print_row(const struct op *op)
{
    enum ui_role role = op->sign == '-' ? UI_ERROR : op->sign == '+' ? UI_OK : UI_DIM;
    int budget = ui_columns() - 6;
    if (budget < 8)
        budget = 8;

    char text[1024];
    diff_text(op->p, op->n, text, sizeof text);

    size_t skip = 0;
    size_t fit = ui_wrap_row(text, strlen(text), (size_t)budget, &skip, NULL);

    ui_put("    ");
    ui_esc(ui_style(role));
    ui_putn(&op->sign, 1);
    ui_put(" ");
    ui_putn(text, fit);
    if (text[fit])
        ui_put("…");
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static void print_note(const char *text)
{
    ui_put("    ");
    ui_esc(ui_style(UI_DIM));
    ui_put(text);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

struct patch {
    char  *buf;
    size_t len, cap;
};

static void patch_add(struct patch *p, const char *s, size_t n)
{
    if (!p->buf && p->cap)
        return;
    if (p->len + n + 1 > p->cap) {
        size_t want = p->cap ? p->cap * 2 : 4096;
        while (want < p->len + n + 1)
            want *= 2;
        char *grown = realloc(p->buf, want);
        if (!grown) {
            free(p->buf);
            p->buf = NULL;
            p->cap = 1;
            return;
        }
        p->buf = grown;
        p->cap = want;
    }
    memcpy(p->buf + p->len, s, n);
    p->len += n;
    p->buf[p->len] = '\0';
}

// The ops as patch text: the changed lines with their context, hunks split
// where the file was skipped over. What is worth showing is decided here;
// how wide it is drawn is decided every time it is drawn.
static char *patch_of_ops(const struct op *ops, int nops, const char *path)
{
    char *show = calloc((size_t)nops, 1);
    if (!show)
        return NULL;
    int any = 0;
    for (int k = 0; k < nops; k++) {
        if (ops[k].sign == ' ')
            continue;
        any = 1;
        int from = k - CONTEXT < 0 ? 0 : k - CONTEXT;
        for (int m = from; m <= k + CONTEXT && m < nops; m++)
            show[m] = 1;
    }
    if (!any) {
        free(show);
        return NULL;
    }

    struct patch p = {0};
    if (path && *path) {
        patch_add(&p, "@@file ", 7);
        patch_add(&p, path, strlen(path));
        patch_add(&p, "\n", 1);
    }

    int open = 0, gap = 1;
    for (int k = 0; k < nops; k++) {
        if (!show[k]) {
            gap = 1;
            continue;
        }
        if (gap || !open) {
            patch_add(&p, "@@\n", 3);
            open = 1;
            gap = 0;
        }
        patch_add(&p, &ops[k].sign, 1);
        patch_add(&p, ops[k].p, ops[k].n);
        patch_add(&p, "\n", 1);
    }
    free(show);
    return p.buf;
}

static char *patch_diff(const char *a_text, size_t a_len, const char *b_text, size_t b_len,
                        const char *path)
{
    struct linevec a, b;
    if (!lines_split(&a, a_text, a_len))
        return NULL;
    if (!lines_split(&b, b_text, b_len)) {
        lines_free(&a);
        return NULL;
    }

    int limit = a.count < b.count ? a.count : b.count;
    int pre = 0;
    while (pre < limit && line_eq(&a, pre, &b, pre))
        pre++;
    int post = 0;
    while (post < limit - pre && line_eq(&a, a.count - 1 - post, &b, b.count - 1 - post))
        post++;

    int ra = a.count - post - pre;
    int rb = b.count - post - pre;

    struct op *ops = NULL;
    int nops = 0, cap = 0;

    op_block(&ops, &nops, &cap, ' ', &a, pre - CONTEXT < 0 ? 0 : pre - CONTEXT, pre);

    if ((long)ra * (long)rb > LCS_BUDGET || !lcs_script(&ops, &nops, &cap, &a, &b, pre, ra, rb)) {
        op_block(&ops, &nops, &cap, '-', &a, pre, pre + ra);
        op_block(&ops, &nops, &cap, '+', &b, pre, pre + rb);
    }

    int tail = a.count - post;
    op_block(&ops, &nops, &cap, ' ', &a, tail, tail + CONTEXT < a.count ? tail + CONTEXT : a.count);

    char *patch = nops > 0 ? patch_of_ops(ops, nops, path) : NULL;

    free(ops);
    lines_free(&a);
    lines_free(&b);
    return patch;
}

int filediff_patch_draws(const char *patch)
{
    if (!patch)
        return 0;
    for (const char *p = patch; *p;) {
        if (*p == '+' || *p == '-')
            return 1;
        const char *nl = strchr(p, '\n');
        if (!nl)
            break;
        p = nl + 1;
    }
    return 0;
}

int filediff_render_patch(const char *patch)
{
    if (!patch || !*patch)
        return 0;

    struct linevec lines;
    if (!lines_split(&lines, patch, strlen(patch)))
        return 0;

    int in_hunk = 0, rows = 0, changed = 0, dropped = 0;
    for (int i = 0; i < lines.count; i++) {
        const char *p = lines.p[i];
        size_t n = lines.n[i];

        if (n >= 7 && memcmp(p, "@@file ", 7) == 0) {
            in_hunk = 0;
            if (rows < MAX_ROWS) {
                char path[1024];
                diff_text(p + 7, n - 7, path, sizeof path);
                print_note(path);
            }
            continue;
        }
        if (n >= 2 && p[0] == '@' && p[1] == '@') {
            // A hunk after another is a stretch of the file passed over.
            if (in_hunk && rows > 0 && rows < MAX_ROWS)
                print_note("\xe2\x8b\xae");
            in_hunk = 1;
            continue;
        }
        if (!in_hunk || n == 0 || (p[0] != ' ' && p[0] != '+' && p[0] != '-'))
            continue;

        if (p[0] == '+' || p[0] == '-')
            changed++;
        if (rows >= MAX_ROWS) {
            if (p[0] == '+' || p[0] == '-')
                dropped++;
            continue;
        }
        struct op op = { .sign = p[0], .p = p + 1, .n = n - 1 };
        print_row(&op);
        rows++;
    }
    lines_free(&lines);

    if (dropped > 0) {
        char note[64];
        snprintf(note, sizeof note, "+%d more line%s", dropped,
                 dropped == 1 ? "" : "s");
        print_note(note);
    }
    return changed > 0;
}

void filediff_clear(void)
{
    free(snap.before);
    snap.before = NULL;
    snap.before_len = 0;
    snap.path[0] = '\0';
    snap.have = 0;
}

void filediff_snapshot(const char *path)
{
    filediff_clear();
    if (!path || !*path)
        return;

    struct stat st;
    if (stat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode) || (size_t)st.st_size > MAX_BYTES)
            return;
        snap.before = text_slurp(path, MAX_BYTES, &snap.before_len);
        if (!snap.before)
            return;
    } else {
        snap.before = calloc(1, 1);
        if (!snap.before)
            return;
        snap.before_len = 0;
    }

    snprintf(snap.path, sizeof snap.path, "%s", path);
    snap.have = 1;
}

char *filediff_take_patch(void)
{
    if (!snap.have)
        return NULL;

    size_t after_len = 0;
    char *after = text_slurp(snap.path, MAX_BYTES, &after_len);
    char *patch = NULL;
    if (after && !(after_len == snap.before_len && memcmp(after, snap.before, after_len) == 0))
        // No file name: the tool call above it already says which file.
        patch = patch_diff(snap.before, snap.before_len, after, after_len, NULL);

    free(after);
    filediff_clear();
    return patch;
}
