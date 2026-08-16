
#ifndef GITINFO_H
#define GITINFO_H

struct gitinfo {
    int  repo;
    char branch[128];
    char sha[24];
    long added, removed;
    int  dirty;
    int  untracked;
};

const struct gitinfo *gitinfo_get(const char *dir);

/* Drop the cached read, for the points where something just ran that can have
 * changed the tree. Without it the next paint can be up to a TTL out of date. */
void gitinfo_forget(void);

#endif
