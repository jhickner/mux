#ifndef BANNER_H
#define BANNER_H

struct session;

/* The two-line identity block: name + model, then the hint row. */
void banner_print(const struct session *s);

/* Just the identity row, for reprinting after a model or session change. */
void banner_identity(const struct session *s);

/* Just the hint row, for when the identity has already been printed. */
void banner_hints(void);

#endif /* BANNER_H */
