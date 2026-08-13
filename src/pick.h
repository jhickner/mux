/* A one-of-N chooser painted in scrollback, navigated with the arrow keys. */
#ifndef PICK_H
#define PICK_H

struct pick_item {
    const char *label;
    const char *detail; /* shown dim beside the label; may be NULL */
};

/* Show `title` over `items` and return the chosen index, or -1 if the user
 * cancelled with Esc or Ctrl-C. The block is erased before returning. */
int pick(const char *title, const struct pick_item *items, int count, int initial);

#endif /* PICK_H */
