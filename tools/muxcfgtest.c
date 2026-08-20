// The matrix store: named configs, per-row prompts, and the older settings line.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "muxcfg.h"
#include "pick.h"
#include "settings.h"

static const struct pick_item NONE[] = {{"default", NULL}};

const struct pick_item *cmd_model_choices(const char *backend, int *count);
const struct pick_item *cmd_effort_choices(const char *backend, int *count);
int pick_run(const char *title, const struct pick_item *items, int count, int initial);
int pick_run_keys(const char *title, const struct pick_item *items, int count,
                  int initial, const char *shortcuts, int *pressed);
char *ask_run(const char *title, const char *initial);
int muxmake_run(const char *description);
const char *session_saved_model(const char *backend);
const char *session_saved_effort(const char *backend);
void ui_note(const char *fmt, ...);
void ui_error(const char *fmt, ...);
void ui_put(const char *s);
void ui_flush(void);

const struct pick_item *cmd_model_choices(const char *backend, int *count)
{
    (void)backend;
    *count = 1;
    return NONE;
}
const struct pick_item *cmd_effort_choices(const char *backend, int *count)
{
    (void)backend;
    *count = 1;
    return NONE;
}
int pick_run(const char *title, const struct pick_item *items, int count, int initial)
{
    (void)title, (void)items, (void)count, (void)initial;
    return -1;
}
int pick_run_keys(const char *title, const struct pick_item *items, int count,
                  int initial, const char *shortcuts, int *pressed)
{
    (void)title, (void)items, (void)count, (void)initial, (void)shortcuts;
    if (pressed)
        *pressed = 0;
    return -1;
}
char *ask_run(const char *title, const char *initial)
{
    (void)title, (void)initial;
    return NULL;
}
int muxmake_run(const char *description)
{
    (void)description;
    return 0;
}
const char *session_saved_model(const char *backend)
{
    return strcmp(backend, "claude") ? NULL : "claude-opus-5";
}
const char *session_saved_effort(const char *backend)
{
    return strcmp(backend, "claude") ? NULL : "high";
}
void ui_note(const char *fmt, ...) { (void)fmt; }
void ui_error(const char *fmt, ...) { (void)fmt; }
void ui_put(const char *s) { (void)s; }
void ui_flush(void) {}

// The store is read once per process, so each case gets its own.
static void in_home(const char *name, void (*body)(const char *home))
{
    char home[256];
    snprintf(home, sizeof home, "/tmp/muxcfgtest-%s", name);

    char rm[512];
    snprintf(rm, sizeof rm, "rm -rf %s", home);
    assert(system(rm) == 0);
    assert(mkdir(home, 0700) == 0);
    setenv("HOME", home, 1);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        body(home);
        _exit(0);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(system(rm) == 0);
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

// No store yet: the matrix comes from the settings line the first cut wrote.
static void case_legacy(const char *home)
{
    char path[512];
    snprintf(path, sizeof path, "%s/settings", home);
    write_file(path, SETTING_MUX_BACKENDS "=claude,codex:gpt-5.6-sol:high,nonesuch\n");
    settings_open(path);

    struct mux_spec v[MUX_MAX];
    int             n = muxcfg_load(v, MUX_MAX);

    assert(n == 2);
    assert(!strcmp(v[0].backend, "claude"));
    assert(!strcmp(v[0].model, "claude-opus-5"));
    assert(!strcmp(v[0].effort, "high"));
    assert(!*v[0].prompt);
    assert(!strcmp(v[1].backend, "codex"));
    assert(!strcmp(v[1].model, "gpt-5.6-sol"));
    assert(!strcmp(v[1].effort, "high"));
    assert(!strcmp(muxcfg_active(), "default"));
}

// A store with two named matrices: the active one is what runs.
static void case_configs(const char *home)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s/.config", home);
    assert(mkdir(dir, 0700) == 0);
    snprintf(dir, sizeof dir, "%s/.config/mux", home);
    assert(mkdir(dir, 0700) == 0);

    char path[512];
    snprintf(path, sizeof path, "%s/matrix.json", dir);
    write_file(path,
               "{\"active\":\"review\",\"configs\":{"
               "\"default\":[{\"backend\":\"claude\",\"model\":\"claude-opus-5\","
               "\"effort\":\"\",\"prompt\":\"\"}],"
               "\"review\":[{\"backend\":\"codex\",\"model\":\"gpt-5.6-sol\","
               "\"effort\":\"high\",\"prompt\":\"answer in one paragraph\"},"
               "{\"backend\":\"bogus\",\"model\":\"\",\"effort\":\"\",\"prompt\":\"\"},"
               "{\"backend\":\"grok\",\"model\":\"\",\"effort\":\"\",\"prompt\":\"\"}]}}");

    struct mux_spec v[MUX_MAX];
    int             n = muxcfg_load(v, MUX_MAX);

    assert(!strcmp(muxcfg_active(), "review"));
    assert(n == 2);
    assert(!strcmp(v[0].backend, "codex"));
    assert(!strcmp(v[0].prompt, "answer in one paragraph"));
    assert(!strcmp(v[1].backend, "grok"));
}

// The label names a row without repeating what the store spells as empty.
static void case_labels(const char *home)
{
    (void)home;
    struct mux_spec m = {"claude", "claude-opus-5", "high", ""};
    char            label[160];

    muxcfg_label(&m, label, sizeof label);
    assert(!strcmp(label, "claude \xc2\xb7 opus-5 \xc2\xb7 high"));

    struct mux_spec bare = {"codex", "", "", ""};
    muxcfg_label(&bare, label, sizeof label);
    assert(!strcmp(label, "codex \xc2\xb7 default"));
}

int main(void)
{
    in_home("legacy", case_legacy);
    in_home("configs", case_configs);
    in_home("labels", case_labels);
    printf("ok\n");
    return 0;
}
