
#ifndef MUXMAKE_H
#define MUXMAKE_H

// Builds a named matrix from a description ("three haikus, a rabbi, a priest,
// and an atheist") by asking a model for one, and makes it the one in use.
// Returns the rows installed, or 0 if nothing usable came back.
int muxmake_run(const char *description);

#endif
