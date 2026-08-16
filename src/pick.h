
#ifndef PICK_H
#define PICK_H

struct pick_item {
    const char *label;
    const char *detail;
};

int pick_run(const char *title, const struct pick_item *items, int count, int initial);

#endif
