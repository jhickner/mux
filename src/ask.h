
#ifndef ASK_H
#define ASK_H

// Ask for one line of text, modally. Returns a malloc'd string, or NULL if it
// was escaped out of. `initial` seeds the field and may be NULL.
char *ask_run(const char *title, const char *initial);

#endif
