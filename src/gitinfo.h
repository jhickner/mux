
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

void gitinfo_forget(void);

#endif
