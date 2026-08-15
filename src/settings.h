/* Settings that outlive a run, one `key=value` line each in
 * ~/.config/mux/settings. */
#ifndef SETTINGS_H
#define SETTINGS_H

#define SETTING_THINKING   "thinking"
#define SETTING_PERMISSION "permission"
#define SETTING_COMPACT    "compact"
#define SETTING_STICKY     "sticky"
#define SETTING_IMAGE_ROWS "image_rows"
/* TEMP: the keybound color try-out's picks, one swatch name each. */
#define SETTING_COLOR_INPUT    "color_input"
#define SETTING_COLOR_EMPHASIS "color_emphasis"

/* Longest key a setting can carry, for callers that build one. */
#define MAX_SETTING_KEY    64

/* Read `path`, if it is there. Without this every get returns its fallback and
 * nothing is written back. */
void settings_open(const char *path);

int  settings_get_int(const char *key, int fallback);

/* Valid until the next set of the same key. `fallback` is returned as-is. */
const char *settings_get_str(const char *key, const char *fallback);

/* Store `value` and rewrite the file. A value with a newline in it would not
 * survive the round trip, so callers pass single-line strings. */
void settings_set_int(const char *key, int value);
void settings_set_str(const char *key, const char *value);

#endif /* SETTINGS_H */
