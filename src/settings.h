/* Settings that outlive a run, one `key=value` line each in
 * ~/.config/simple-agent/settings. */
#ifndef SETTINGS_H
#define SETTINGS_H

#define SETTING_THINKING   "thinking"
#define SETTING_PERMISSION "permission"

/* Read `path`, if it is there. Without this every get returns its fallback and
 * nothing is written back. */
void settings_open(const char *path);

int  settings_get_int(const char *key, int fallback);

/* Store `value` and rewrite the file. */
void settings_set_int(const char *key, int value);

#endif /* SETTINGS_H */
