#include "files.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define INDEX_MAX  20000 /* entries kept; a huge tree is simply truncated */
#define INDEX_TTL  10    /* seconds an index is reused before rebuilding */
#define WALK_DEPTH 8
#define TOP_MAX    64    /* candidates ranked at once (repl's dropdown cap) */

/* Directories the fallback walk never descends into. Repos with a .gitignore
 * are handled by git ls-files instead. */
static const char *SKIP_DIRS[] = {"node_modules", "target", "build", "dist",
                                  "vendor",       "venv",   "__pycache__"};

struct index {
    char **paths; /* relative; directories carry a trailing '/' */
    int    count, cap;
    char  *root;
    time_t built;
};

static struct index IDX;

/* ---------- building ---------- */

static void index_clear(struct index *ix)
{
    for (int i = 0; i < ix->count; i++)
        free(ix->paths[i]);
    ix->count = 0;
}

static void index_add(struct index *ix, const char *path)
{
    if (ix->count >= INDEX_MAX || !*path)
        return;
    if (ix->count == ix->cap) {
        int cap = ix->cap ? ix->cap * 2 : 256;
        char **grown = realloc(ix->paths, (size_t)cap * sizeof *grown);
        if (!grown)
            return;
        ix->paths = grown;
        ix->cap = cap;
    }
    char *d = strdup(path);
    if (d)
        ix->paths[ix->count++] = d;
}

/* Add the file plus every directory above it, so "@src/" narrows before a file
 * is chosen. Duplicate parents are removed when the index is sorted. */
static void index_add_with_parents(struct index *ix, const char *path)
{
    for (const char *s = strchr(path, '/'); s; s = strchr(s + 1, '/')) {
        char dir[4096];
        size_t n = (size_t)(s - path) + 1;
        if (n >= sizeof dir)
            break;
        memcpy(dir, path, n);
        dir[n] = '\0';
        index_add(ix, dir);
    }
    index_add(ix, path);
}

static int path_depth(const char *s)
{
    int n = 0;
    for (; *s && s[1]; s++) /* a trailing '/' marks a directory, not a level */
        if (*s == '/')
            n++;
    return n;
}

/* Shallow before deep, then alphabetical: the order the empty "@" list shows. */
static int cmp_path(const void *a, const void *b)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    int dx = path_depth(x), dy = path_depth(y);
    return dx != dy ? dx - dy : strcmp(x, y);
}

static void index_sort_unique(struct index *ix)
{
    if (ix->count < 2)
        return;
    qsort(ix->paths, (size_t)ix->count, sizeof *ix->paths, cmp_path);
    int w = 1;
    for (int i = 1; i < ix->count; i++) {
        if (strcmp(ix->paths[i], ix->paths[w - 1]) == 0)
            free(ix->paths[i]);
        else
            ix->paths[w++] = ix->paths[i];
    }
    ix->count = w;
}

/* git knows the tracked and untracked-but-not-ignored files, which is exactly
 * the set worth offering. Returns 0 when `root` is not a repo. */
static int index_from_git(struct index *ix, const char *root)
{
    if (strchr(root, '\'')) /* unquotable root: fall back to the walk */
        return 0;
    char cmd[4200];
    snprintf(cmd, sizeof cmd,
             "git -C '%s' ls-files --cached --others --exclude-standard 2>/dev/null", root);
    FILE *f = popen(cmd, "r");
    if (!f)
        return 0;

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        if (line[n - 1] == '\n')
            line[--n] = '\0';
        /* git C-quotes paths with unusual bytes; those are not worth unquoting. */
        if (n > 0 && line[0] != '"')
            index_add_with_parents(ix, line);
    }
    free(line);
    pclose(f);
    return ix->count > 0;
}

static int is_skipped_dir(const char *name)
{
    for (size_t i = 0; i < sizeof SKIP_DIRS / sizeof *SKIP_DIRS; i++)
        if (strcmp(name, SKIP_DIRS[i]) == 0)
            return 1;
    return 0;
}

/* `abs` is the directory to read, `rel` its path relative to the root ("" at the
 * top, otherwise slash-terminated). */
static void walk(struct index *ix, const char *abs, const char *rel, int depth)
{
    if (depth > WALK_DEPTH || ix->count >= INDEX_MAX)
        return;
    DIR *d = opendir(abs);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && ix->count < INDEX_MAX) {
        if (e->d_name[0] == '.')
            continue;
        char child_abs[4096], child_rel[4096];
        snprintf(child_abs, sizeof child_abs, "%s/%s", abs, e->d_name);

        int dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) {
            struct stat st;
            dir = stat(child_abs, &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (dir && is_skipped_dir(e->d_name))
            continue;
        snprintf(child_rel, sizeof child_rel, "%s%s%s", rel, e->d_name, dir ? "/" : "");
        index_add(ix, child_rel);
        if (dir)
            walk(ix, child_abs, child_rel, depth + 1);
    }
    closedir(d);
}

static void index_build(struct index *ix, const char *root)
{
    index_clear(ix);
    if (!ix->root || strcmp(ix->root, root) != 0) {
        free(ix->root);
        ix->root = strdup(root);
    }
    ix->built = time(NULL);
    if (!index_from_git(ix, root))
        walk(ix, root, "", 0);
    index_sort_unique(ix);
}

void files_forget(void)
{
    index_clear(&IDX);
    free(IDX.paths);
    free(IDX.root);
    memset(&IDX, 0, sizeof IDX);
}

/* ---------- ranking ---------- */

/* Case-insensitive subsequence score, or -1 when `q` is not a subsequence.
 * Contiguous runs and matches at a word boundary score higher. */
static int fuzzy(const char *name, const char *q)
{
    int score = 0, ni = 0, streak = 0;
    for (int qi = 0; q[qi]; qi++) {
        int qc = tolower((unsigned char)q[qi]);
        int found = 0;
        while (name[ni]) {
            char prev = ni ? name[ni - 1] : '/';
            int nc = tolower((unsigned char)name[ni]);
            ni++;
            if (nc == qc) {
                streak++;
                score += 1 + streak;
                if (prev == '/' || prev == '_' || prev == '-' || prev == '.')
                    score += 4;
                found = 1;
                break;
            }
            streak = 0;
        }
        if (!found)
            return -1;
    }
    return score;
}

/* Offset of the last path segment, ignoring a directory's trailing '/'. */
static int basename_at(const char *path)
{
    int end = (int)strlen(path);
    if (end > 0 && path[end - 1] == '/')
        end--;
    for (int i = end - 1; i >= 0; i--)
        if (path[i] == '/')
            return i + 1;
    return 0;
}

/* A token without a slash is meant for the file name, so a basename hit beats a
 * scattered match across the directories above it. Shorter paths break ties. */
static int path_score(const char *path, const char *token)
{
    int         s = -1;
    const char *base = path + basename_at(path);
    if (!strchr(token, '/'))
        s = fuzzy(base, token);
    if (s >= 0) {
        s += 20;
        /* A name starting with what was typed beats the same letters scattered
         * through a longer one, and a name that *is* what was typed beats both:
         * "@src" should offer src/ ahead of src/sessionfork.c. */
        size_t tl = strlen(token), bl = strlen(base);
        if (tl && strncasecmp(base, token, tl) == 0) {
            s += 30;
            if (bl == tl || (bl == tl + 1 && base[tl] == '/'))
                s += 30;
        }
    } else {
        s = fuzzy(path, token);
    }
    if (s < 0)
        return -1;
    s = s * 4 - (int)strlen(path) / 4;
    return s < 0 ? 0 : s;
}

int files_complete(void *ctx, const char *token, ReplCandidate *out, int max)
{
    const char *root = ctx ? ctx : ".";
    time_t now = time(NULL);
    if (!IDX.root || strcmp(IDX.root, root) != 0 || now - IDX.built > INDEX_TTL)
        index_build(&IDX, root);

    int cap = max < TOP_MAX ? max : TOP_MAX;
    if (cap <= 0)
        return 0;

    /* Keep the best `cap` by insertion, so a large index costs no allocation. */
    int idx[TOP_MAX], score[TOP_MAX], n = 0;
    for (int i = 0; i < IDX.count; i++) {
        /* An exact hit completes to what is already typed. Dropping it keeps a
         * just-accepted directory off the top of its own narrowed list, where
         * Tab would otherwise re-accept it and appear to do nothing. */
        if (strcmp(IDX.paths[i], token) == 0)
            continue;
        int sc = *token ? path_score(IDX.paths[i], token) : 0;
        if (sc < 0)
            continue;
        if (n == cap) {
            if (sc <= score[cap - 1])
                continue;
            n = cap - 1;
        }
        int j = n++;
        while (j > 0 && score[j - 1] < sc) {
            score[j] = score[j - 1];
            idx[j] = idx[j - 1];
            j--;
        }
        score[j] = sc;
        idx[j] = i;
    }

    for (int k = 0; k < n; k++) {
        snprintf(out[k].text, REPL_CAND_TEXT, "%s", IDX.paths[idx[k]]);
        out[k].desc[0] = '\0';
    }
    return n;
}
