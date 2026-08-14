/* The repository state behind the HUD's location row, read by shelling out to
 * git and cached: the row repaints on every keystroke and spinner tick, which
 * is far more often than a work tree changes. */
#ifndef GITINFO_H
#define GITINFO_H

struct gitinfo {
    int  repo;          /* the directory is inside a work tree */
    char branch[128];   /* empty when HEAD is detached */
    char sha[24];
    long added, removed; /* lines against HEAD, staged and not */
    int  dirty;          /* tracked files changed, staged or not */
    int  untracked;      /* files git does not know about, ignored ones aside */
};

/* The state of `dir`, re-read at most every few seconds. Never NULL. */
const struct gitinfo *gitinfo_get(const char *dir);

#endif /* GITINFO_H */
