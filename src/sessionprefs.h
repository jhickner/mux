
#ifndef SESSIONPREFS_H
#define SESSIONPREFS_H

const char *prefs_saved_choice(const char *what, const char *backend);
void        prefs_remember_choice(const char *what, const char *backend, const char *value);

const char *prefs_resolved_model(const char *backend, const char *model);
void        prefs_remember_resolved_model(const char *backend, const char *model,
                                          const char *resolved);

long prefs_window(const char *backend, const char *model_label);
void prefs_remember_window(const char *backend, const char *model_label, long window);

#endif
