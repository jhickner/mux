
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

// How a list is searched, for the ones that can be.
enum pick_search {
    PICK_SEARCH_TYPE,   /* every typed letter narrows the list */
    PICK_SEARCH_SLASH,  /* '/' opens a query; until then letters are shortcuts */
};

// A shortcuts string may hold '\n' for shift-enter, or PICK_KEY_RIGHT for the
// right arrow; both come back in *pressed the same way.
#define PICK_KEY_RIGHT '\x1c'

// Values for pick_live.heading below.
#define PICK_HEADING 1   /* a group header: dim, never selectable, dropped by
                            a query that empties its group */
#define PICK_APART   2   /* an ordinary row, set off by a blank line above */

//
// A list whose rows say what they are doing while it is open.
struct pick_live {
    // What sets item i apart from the row above it: PICK_HEADING makes it a
    // group header, PICK_APART leaves it an ordinary row with a blank line
    // over it. Zero for everything else.
    const unsigned char *heading;
    // A status column at the front of every other row: a turning spinner
    // where spin[i] is set, otherwise the still mark[i], where there is one.
    // A mark is drawn in role[i] when roles are given, and as an error when
    // they are not.
    const unsigned char *spin;
    const char *const   *mark;
    const unsigned char *mark_role;
    // Called on the spinner's frames. Nonzero means the caller has changed
    // what the rows say, and the list is drawn again.
    int  (*tick)(void *ud);
    void  *ud;
};

// A live list, grouped, with the status column above. The keys in `shortcuts`
// close it: the highlighted row comes back, with the key that took it in
// *pressed (0 for enter). A list whose shortcuts are letters wants
// PICK_SEARCH_SLASH, so that typing one means the key and not the letter.
int pick_run_live(const char *title, const struct pick_item *items, int count,
                  int initial, const struct pick_live *live,
                  enum pick_search search, const char *shortcuts, int *pressed);

int pick_run_keys(const char *title, const struct pick_item *items, int count,
                  int initial, const char *shortcuts, int *pressed);

#endif
