#include "title.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "vendor/agents/backend.h"

#define CLAUDE_TITLE_MODEL "claude-haiku-4-5-20251001"
#define TITLE_MAX   80
/* Enough of the turn to name it by; the rest is repetition for this purpose. */
#define EXCERPT     1200

static int cache_path(char *out, size_t size)
{
    const char *home = getenv("HOME");
    if (!home)
        return 0;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/.config/simple-agent", home);
    mkdir(dir, 0700);
    snprintf(out, size, "%s/titles", dir);
    return 1;
}

int title_lookup(const char *id, char *out, size_t size)
{
    char path[1200];
    if (!id || !*id || !cache_path(path, sizeof path))
        return 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    char *line = NULL;
    size_t cap = 0;
    size_t id_len = strlen(id);
    int found = 0;
    /* Last match wins: a retitle is appended rather than rewritten in place. */
    while (getline(&line, &cap, f) > 0) {
        if (strncmp(line, id, id_len) != 0 || line[id_len] != '\t')
            continue;
        char *text = line + id_len + 1;
        text[strcspn(text, "\n")] = '\0';
        if (*text) {
            snprintf(out, size, "%s", text);
            found = 1;
        }
    }
    free(line);
    fclose(f);
    return found;
}

/* Take the first line, drop the quoting and trailing punctuation a model adds
 * around a title, and refuse anything that reads like prose instead. */
static int tidy(char *text)
{
    text[strcspn(text, "\n")] = '\0';
    char *p = text;
    while (*p == ' ' || *p == '"' || *p == '\'')
        p++;
    size_t n = strlen(p);
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '.' || p[n - 1] == '"' || p[n - 1] == '\''))
        p[--n] = '\0';
    if (n == 0 || n > TITLE_MAX)
        return 0;
    memmove(text, p, n + 1);
    return 1;
}

static void write_cache(const char *id, const char *title)
{
    char path[1200];
    if (!cache_path(path, sizeof path))
        return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return;
    char row[256];
    int n = snprintf(row, sizeof row, "%s\t%s\n", id, title);
    /* One write of one short row: concurrent agents append without interleaving. */
    if (n > 0)
        (void)!write(fd, row, (size_t)n);
    close(fd);
}

static void ask_and_cache(const char *id, const char *backend, const char *model,
                          const char *cwd, const char *prompt, const char *reply)
{
    char text[2 * EXCERPT + 256];
    snprintf(text, sizeof text,
             "Name this conversation in 3 to 6 words: what it is about, in sentence case, "
             "no quotes and no final period. Reply with the title alone.\n\n"
             "Asked:\n%.*s\n\nAnswered:\n%.*s\n",
             EXCERPT, prompt ? prompt : "", EXCERPT, reply ? reply : "");

    backend_opts o = {0};
    o.name = backend;
    o.model = (!backend || strcmp(backend, "claude") == 0) ? CLAUDE_TITLE_MODEL : model;
    o.cwd = cwd;
    o.system = "Name the conversation without using tools.";
    o.session_name = "simple-agent title helper";
    o.ephemeral = 1;
    o.disable_tools = 1;
    Backend *b = backend_open_ex(&o);
    if (!b)
        return;
    char *answer = b->ask(b, text);
    if (answer && tidy(answer))
        write_cache(id, answer);
    free(answer);
    b->close(b);
}

void title_request(const char *id, const char *backend, const char *model,
                   const char *cwd, const char *prompt, const char *reply)
{
    if (!id || !*id)
        return;

    /* Fork twice: the middle child exits at once, so init reaps the worker and
     * the display never waits on a second CLI starting up. */
    pid_t pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            int null = open("/dev/null", O_RDWR);
            if (null >= 0) {
                dup2(null, STDIN_FILENO);
                dup2(null, STDOUT_FILENO);
                dup2(null, STDERR_FILENO);
                if (null > STDERR_FILENO)
                    close(null);
            }
            ask_and_cache(id, backend, model, cwd, prompt, reply);
        }
        _exit(0);
    }
    waitpid(pid, NULL, 0);
}
