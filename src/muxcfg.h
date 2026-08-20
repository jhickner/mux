
#ifndef MUXCFG_H
#define MUXCFG_H

#include <stddef.h>

#define MUX_MAX      12
#define MUX_SETS     8
#define MUX_NAME     48
#define MUX_PROMPT   512

// One cell on the y axis: a backend, the model and effort it answers with, and
// the standing instructions it answers under.
struct mux_spec {
    char backend[32];
    char model[128];
    char effort[32];
    char prompt[MUX_PROMPT];
};

// The rows of the matrix in use, and its name.
int         muxcfg_load(struct mux_spec *out, int max);
const char *muxcfg_active(void);

void muxcfg_label(const struct mux_spec *m, char *out, size_t cap);

void muxcfg_run(void);

// Takes a matrix built elsewhere, names it, and makes it the one in use.
// Rows on backends that are not installed are dropped. Returns rows kept.
int muxcfg_install(const char *name, const struct mux_spec *v, int n);

#endif
