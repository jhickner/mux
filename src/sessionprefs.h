
#ifndef SESSIONPREFS_H
#define SESSIONPREFS_H

/* Settings-key policy for the per-backend choices a session remembers. Kept
   apart from session.c so the key layout lives in one place and the session
   itself only deals in plain strings. */

/* "<what>.<backend>", where what is "model" or "effort". NULL when unset or
   explicitly "default". */
const char *prefs_saved_choice(const char *what, const char *backend);
void        prefs_remember_choice(const char *what, const char *backend, const char *value);

/* "model.<backend>.<model>" -> the id the CLI actually resolved to. */
const char *prefs_resolved_model(const char *backend, const char *model);
void        prefs_remember_resolved_model(const char *backend, const char *model,
                                          const char *resolved);

/* "window.<backend>.<model label>" -> that model's context window. */
long prefs_window(const char *backend, const char *model_label);
void prefs_remember_window(const char *backend, const char *model_label, long window);

#endif
