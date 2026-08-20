
#ifndef SETTINGS_H
#define SETTINGS_H

#define SETTING_THINKING   "thinking"
#define SETTING_PERMISSION "permission"
#define SETTING_COMPACT    "compact"
#define SETTING_STICKY     "sticky"
#define SETTING_IMAGE_ROWS "image_rows"
#define SETTING_ECHO_ROWS  "echo_rows"

#define ECHO_ROWS_DEFAULT 10

#define SETTING_MUX_BACKENDS "mux_backends"

#define SETTING_COLOR_INPUT    "color_input"
#define SETTING_COLOR_EMPHASIS "color_emphasis"

#define MAX_SETTING_KEY    64
#define MAX_SETTING_VALUE  256

void settings_open(const char *path);

int  settings_get_int(const char *key, int fallback);

const char *settings_get_str(const char *key, const char *fallback);

void settings_set_int(const char *key, int value);
void settings_set_str(const char *key, const char *value);

#endif
