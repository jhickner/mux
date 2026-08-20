
#ifndef MODELS_H
#define MODELS_H

struct pick_item;

// The models a backend can be asked for, read from that CLI's own catalog
// where it keeps one. The list ends with "default", and points at storage the
// module owns.
int models_for(const char *backend, const struct pick_item **out);

#endif
