#include "sessionprefs.h"

#include <stdio.h>
#include <string.h>

#include "settings.h"

static void choice_key(const char *what, const char *backend, char *out, size_t n)
{
    snprintf(out, n, "%s.%s", what, backend);
}

static void model_key(const char *backend, const char *model, char *out, size_t n)
{
    snprintf(out, n, "model.%s.%s", backend, model && *model ? model : "default");
}

static void window_key(const char *backend, const char *label, char *out, size_t n)
{
    snprintf(out, n, "window.%s.%s", backend, label);
}

const char *prefs_saved_choice(const char *what, const char *backend)
{
    if (!backend || !*backend)
        return NULL;
    char key[MAX_SETTING_KEY];
    choice_key(what, backend, key, sizeof key);
    const char *saved = settings_get_str(key, NULL);
    return saved && strcmp(saved, "default") != 0 ? saved : NULL;
}

void prefs_remember_choice(const char *what, const char *backend, const char *value)
{
    char key[MAX_SETTING_KEY];
    choice_key(what, backend, key, sizeof key);
    settings_set_str(key, value && *value ? value : "default");
}

const char *prefs_resolved_model(const char *backend, const char *model)
{
    char key[MAX_SETTING_KEY];
    model_key(backend, model, key, sizeof key);
    return settings_get_str(key, NULL);
}

void prefs_remember_resolved_model(const char *backend, const char *model,
                                   const char *resolved)
{
    if (!resolved || !*resolved)
        return;
    char key[MAX_SETTING_KEY];
    model_key(backend, model, key, sizeof key);
    settings_set_str(key, resolved);
}

long prefs_window(const char *backend, const char *model_label)
{
    char key[MAX_SETTING_KEY];
    window_key(backend, model_label, key, sizeof key);
    return settings_get_int(key, 0);
}

void prefs_remember_window(const char *backend, const char *model_label, long window)
{
    if (window <= 0)
        return;
    char key[MAX_SETTING_KEY];
    window_key(backend, model_label, key, sizeof key);
    settings_set_int(key, (int)window);
}
