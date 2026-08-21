
#ifndef FRONTEND_H
#define FRONTEND_H

// What the front end that submitted the line being processed can do. The
// default, with nothing pushed, is the terminal: a keyboard is present.
enum {
    FRONTEND_KEYBOARD = 1u << 0,
};

void     frontend_push(unsigned caps);
void     frontend_pop(void);
unsigned frontend_caps(void);
int      frontend_has_keyboard(void);

#endif
