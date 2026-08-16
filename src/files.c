#include "files.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define INDEX_MAX  20000
#define INDEX_TTL  10
#define WALK_DEPTH 8
#define TOP_MAX    64

static const char *SKIP_DIRS[] = {"node_modules", "target", "build", "dist",
                                  "vendor",       "venv",   "__pycache__"};

struct index {
    char **paths;
    int    count, cap;
    char  *root;
    time_t built;
};

static struct index IDX;

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
    for (; *s && s[1]; s++)
        if (*s == '/')
            n++;
    return n;
}

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

static int index_from_git(struct index *ix, const char *root)
{
    if (strchr(root, '\''))
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

static int path_score(const char *path, const char *token)
{
    int         s = -1;
    const char *base = path + basename_at(path);
    if (!strchr(token, '/'))
        s = fuzzy(base, token);
    if (s >= 0) {
        s += 20;

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

static int is_external(const char *t)
{
    if (t[0] == '~' || t[0] == '/')
        return 1;
    if (t[0] != '.')
        return 0;
    if (t[1] == '/' || t[1] == '\0')
        return 1;
    return t[1] == '.' && (t[2] == '/' || t[2] == '\0');
}

static int split_external(const char *token, const char *root, char *prefix, size_t prefix_sz,
                          char *dir, size_t dir_sz, char *base, size_t base_sz)
{
    const char *slash = strrchr(token, '/');
    char        pre[4096];
    if (slash) {
        size_t n = (size_t)(slash - token) + 1;
        if (n >= sizeof pre || strlen(slash + 1) >= base_sz)
            return 0;
        memcpy(pre, token, n);
        pre[n] = '\0';
        snprintf(base, base_sz, "%s", slash + 1);
    } else {

        snprintf(pre, sizeof pre, "%s/", token);
        base[0] = '\0';
    }

    if (pre[0] == '~') {
        const char *home = getenv("HOME");
        if (!home || !*home)
            return 0;
        if (snprintf(prefix, prefix_sz, "%s%s", home, pre + 1) >= (int)prefix_sz)
            return 0;
    } else if (snprintf(prefix, prefix_sz, "%s", pre) >= (int)prefix_sz) {
        return 0;
    }

    if (prefix[0] == '/')
        return snprintf(dir, dir_sz, "%s", prefix) < (int)dir_sz;
    return snprintf(dir, dir_sz, "%s/%s", root, prefix) < (int)dir_sz;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int complete_external(const char *token, const char *root, ReplCandidate *out, int max)
{
    char prefix[4096], dir[4096], base[REPL_CAND_TEXT];
    if (!split_external(token, root, prefix, sizeof prefix, dir, sizeof dir, base, sizeof base))
        return 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;

    char        *names[TOP_MAX * 8];
    int          count = 0;
    struct dirent *e;
    while ((e = readdir(d)) && count < (int)(sizeof names / sizeof *names)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (e->d_name[0] == '.' && base[0] != '.')
            continue;
        char full[4096];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        int isdir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) {
            struct stat st;
            isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        }
        char entry[REPL_CAND_TEXT];
        if (snprintf(entry, sizeof entry, "%s%s", e->d_name, isdir ? "/" : "") >= (int)sizeof entry)
            continue;
        char *dup = strdup(entry);
        if (dup)
            names[count++] = dup;
    }
    closedir(d);
    qsort(names, (size_t)count, sizeof *names, cmp_name);

    int cap = max < TOP_MAX ? max : TOP_MAX;
    int idx[TOP_MAX], score[TOP_MAX], n = 0;
    for (int i = 0; i < count && cap > 0; i++) {
        int sc = base[0] ? fuzzy(names[i], base) : 0;
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
        snprintf(out[k].text, REPL_CAND_TEXT, "%s%s", prefix, names[idx[k]]);
        out[k].desc[0] = '\0';
    }
    for (int i = 0; i < count; i++)
        free(names[i]);
    return n;
}

int files_complete(void *ctx, const char *token, ReplCandidate *out, int max)
{
    const char *root = ctx ? ctx : ".";
    if (is_external(token))
        return complete_external(token, root, out, max);
    time_t now = time(NULL);
    if (!IDX.root || strcmp(IDX.root, root) != 0 || now - IDX.built > INDEX_TTL)
        index_build(&IDX, root);

    int cap = max < TOP_MAX ? max : TOP_MAX;
    if (cap <= 0)
        return 0;

    int idx[TOP_MAX], score[TOP_MAX], n = 0;
    for (int i = 0; i < IDX.count; i++) {

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
