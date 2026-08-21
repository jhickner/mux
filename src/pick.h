
#ifndef PICK_H
#define PICK_H

struct pick_item {
    const char *label;
    const char *detail;
};

int pick_run(const char *title, const struct pick_item *items, int count, int initial);

// As pick_run, but typing narrows the list by fuzzy match on the labels;
// backspace and ctrl-u walk the query back, escape clears it before leaving.
int pick_run_filter(const char *title, const struct pick_item *items, int count, int initial);

// As pick_run, but the keys in `shortcuts` also close the list: the highlighted
// row comes back, with the key that took it in *pressed (0 for enter).
// Both at once: typing narrows the list and the shortcut keys still close it.
int pick_run_ex(const char *title, const struct pick_item *items, int count,
                int initial, const char *shortcuts, int *pressed);

int pick_run_keys(const char *title, const struct pick_item *items, int count,
                  int initial, const char *shortcuts, int *pressed);

#endif
