
#ifndef PICK_H
#define PICK_H

struct pick_item {
    const char *label;
    const char *detail;
};

int pick_run(const char *title, const struct pick_item *items, int count, int initial);

// As pick_run, but the keys in `shortcuts` also close the list: the highlighted
// row comes back, with the key that took it in *pressed (0 for enter).
int pick_run_keys(const char *title, const struct pick_item *items, int count,
                  int initial, const char *shortcuts, int *pressed);

#endif
