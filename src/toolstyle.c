#include "toolstyle.h"

#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "vendor/cJSON.h"

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int same_word(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (lower(*a) != lower(*b))
            return 0;
    return !*a && !*b;
}

static int in_list(const char *const *list, int n, const char *word)
{
    for (int i = 0; i < n; i++)
        if (same_word(list[i], word))
            return 1;
    return 0;
}

static const char *const READ_TOOLS[] = {
    "read", "glob", "grep", "ls", "notebookread",
    "web", "web_search", "websearch", "web_fetch", "webfetch", "fetch",
};

static const char *const SHELL_TOOLS[] = {"bash", "shell", "sh", "exec", "run_command",
                                          "execute_command", "local_shell"};

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static const char *const READERS[] = {
    "cat",  "nl",       "ls",       "eza",   "exa",     "tree",     "find",    "fd",
    "stat", "file",     "pwd",      "du",    "df",      "realpath", "readlink", "which",
    "type", "whereis",  "basename", "dirname", "id",    "whoami",   "hostname", "uname",
    "date", "printenv", "grep",     "egrep", "fgrep",   "rg",       "ag",       "ack",
    "true", "false",
};

static const char *const GIT_READERS[] = {
    "status",   "log",      "diff",     "show",     "blame",    "shortlog",  "describe",
    "rev-parse", "ls-files", "ls-tree", "cat-file", "grep",     "whatchanged", "diff-tree",
};

static const struct {
    const char *name;
    const char *values;
    int         operands;
} FILTERS[] = {
    {"wc", "", 1},        {"sort", "kotSm", 1}, {"uniq", "fsw", 1},   {"cut", "dfbcs", 1},
    {"head", "cn", 1},    {"tail", "cn", 1},    {"tac", "s", 1},      {"rev", "", 1},
    {"fold", "w", 1},     {"expand", "t", 1},   {"unexpand", "t", 1}, {"paste", "d", 1},
    {"comm", "", 1},      {"join", "12teoav", 2}, {"column", "csoN", 1},
    {"echo", "", 0},      {"printf", "", 0},    {"tr", "", 0},        {"sed", "e", 0},
    {"awk", "", 0},
};

static size_t stderr_redirect_end(const char *command, size_t at)
{
    if (at == 0 || command[at - 1] != '2')
        return 0;
    size_t i = at + 1;
    if (command[i] == '&' && command[i + 1] == '1')
        return i + 2;
    while (is_space(command[i]))
        i++;
    if (strncmp(command + i, "/dev/null", 9) == 0)
        return i + 9;
    return 0;
}

static int has_disqualifier(const char *command)
{
    char quote = 0;
    for (size_t i = 0; command[i]; i++) {
        char c = command[i];
        if (quote) {
            if (quote == '"' && c == '\\' && command[i + 1]) {
                i++;
                continue;
            }
            if (c == quote) {
                quote = 0;
                continue;
            }

            if (quote == '"' && (c == '`' || (c == '$' && command[i + 1] == '(')))
                return 1;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (c == '\\' && command[i + 1]) {
            i++;
            continue;
        }
        if (c == '`' || (c == '$' && command[i + 1] == '('))
            return 1;
        if ((c == '<' || c == '>') && command[i + 1] == '(')
            return 1;
        if (c == '<')
            return 1;
        if (c == '&') {
            if (command[i + 1] == '&') {
                i++;
                continue;
            }
            if (i == 0 || command[i - 1] != '>')
                return 1;
        }
        if (c == '>') {
            size_t past = stderr_redirect_end(command, i);
            if (!past)
                return 1;
            i = past - 1;
        }
    }
    return 0;
}

static int take_word(const char **cursor, const char *end, char *out, size_t size)
{
    const char *p = *cursor;
    while (p < end && is_space(*p))
        p++;
    if (p >= end) {
        *cursor = p;
        return 0;
    }

    size_t n = 0;
    char   quote = 0;
    for (; p < end; p++) {
        char c = *p;
        if (quote) {
            if (c == quote) {
                quote = 0;
                continue;
            }
        } else if (c == '\'' || c == '"') {
            quote = c;
            continue;
        } else if (is_space(c)) {
            break;
        } else if (c == '\\' && p + 1 < end) {
            p++;
            c = *p;
        }
        if (n + 1 < size)
            out[n++] = c;
    }
    out[n] = '\0';
    *cursor = p;
    return 1;
}

static int count_operands(const char *values, const char *cursor, const char *end)
{
    char word[512];
    int  operands = 0, want_value = 0;

    while (take_word(&cursor, end, word, sizeof word)) {
        if (want_value) {
            want_value = 0;
            continue;
        }
        if (word[0] != '-' || !word[1]) {
            operands++;
            continue;
        }

        if (word[1] != '-' && strchr(values, word[strlen(word) - 1]))
            want_value = 1;
    }
    return operands;
}

static int is_mutating_search_option(const char *word)
{
    static const char *const NAMES[] = {"-delete", "-exec",   "-execdir", "-ok",
                                        "-okdir",  "-fprint", "-fprint0", "-fprintf",
                                        "-fls",    "--exec",  "--exec-batch"};
    return in_list(NAMES, COUNT(NAMES), word) || strncmp(word, "--exec", 6) == 0;
}

static int sed_script_writes(const char *script)
{
    for (size_t i = 0; script[i]; i++) {
        char c = script[i];
        if (c != 'w' && c != 'W' && c != 'e')
            continue;
        char next = script[i + 1];
        if (next && next != ' ' && next != '\t' && next != ';' && next != '\n' && next != '}')
            continue;
        size_t start = i;
        while (start > 0 && (script[start - 1] == ' ' || script[start - 1] == '\t'))
            start--;
        char before = start > 0 ? script[start - 1] : '\0';
        if (!before || (!(before >= 'a' && before <= 'z') && !(before >= 'A' && before <= 'Z') &&
                        !(before >= '0' && before <= '9') && before != '-'))
            return 1;
    }
    return 0;
}

static int command_writes(const char *name, const char *cursor, const char *end)
{
    int search = same_word(name, "find") || same_word(name, "fd");
    int sed = same_word(name, "sed");
    int awk = same_word(name, "awk");
    if (!search && !sed && !awk)
        return 0;

    char word[512];
    while (take_word(&cursor, end, word, sizeof word)) {
        if (search && is_mutating_search_option(word))
            return 1;
        if ((sed || awk) && strchr(word, '>'))
            return 1;
        if (sed && strncmp(word, "--in-place", 10) == 0)
            return 1;
        if (sed && word[0] == '-' && word[1] != '-') {
            for (size_t i = 1; word[i] && word[i] != '.'; i++)
                if (word[i] == 'i')
                    return 1;
        } else if (sed && sed_script_writes(word)) {
            return 1;
        }
    }
    return 0;
}

enum segment_class {
    SEGMENT_READER,
    SEGMENT_STAGE,
    SEGMENT_UNKNOWN,
};

static enum segment_class classify_segment(const char *segment, const char *end)
{
    char        word[512];
    const char *cursor = segment;
    if (!take_word(&cursor, end, word, sizeof word))
        return SEGMENT_STAGE;

    if (same_word(word, "git")) {
        char sub[512];
        for (;;) {
            if (!take_word(&cursor, end, sub, sizeof sub))
                return SEGMENT_UNKNOWN;
            if (sub[0] != '-')
                break;

            if (!strcmp(sub, "-C") || !strcmp(sub, "-c") || !strcmp(sub, "--git-dir") ||
                !strcmp(sub, "--work-tree") || !strcmp(sub, "--namespace"))
                if (!take_word(&cursor, end, sub, sizeof sub))
                    return SEGMENT_UNKNOWN;
        }
        return in_list(GIT_READERS, COUNT(GIT_READERS), sub) ? SEGMENT_READER : SEGMENT_UNKNOWN;
    }

    if (command_writes(word, cursor, end))
        return SEGMENT_UNKNOWN;

    if (in_list(READERS, COUNT(READERS), word))
        return SEGMENT_READER;

    for (int i = 0; i < COUNT(FILTERS); i++) {
        if (!same_word(FILTERS[i].name, word))
            continue;
        if (FILTERS[i].operands > 0 &&
            count_operands(FILTERS[i].values, cursor, end) >= FILTERS[i].operands)
            return SEGMENT_READER;
        return SEGMENT_STAGE;
    }
    return SEGMENT_UNKNOWN;
}

int toolstyle_shell_reads(const char *command)
{
    if (!command || !*command || has_disqualifier(command))
        return 0;

    const char *segment = command;
    int         reads = 0;
    int         piped_from = 0;
    char        quote = 0;

    for (size_t i = 0;; i++) {
        char c = command[i];
        if (quote) {
            if (c == quote)
                quote = 0;
            else if (c == '\\' && command[i + 1])
                i++;
            if (c)
                continue;
        } else if (c == '\'' || c == '"') {
            quote = c;
            continue;
        } else if (c == '\\' && command[i + 1]) {
            i++;
            continue;
        }

        int pipe = 0, width = 0;
        if ((c == '&' && command[i + 1] == '&') || (c == '|' && command[i + 1] == '|'))
            width = 2;
        else if (c == ';' || c == '\n' || c == '\r')
            width = 1;
        else if (c == '|')
            width = pipe = 1;
        else if (c)
            continue;

        enum segment_class class = classify_segment(segment, command + i);
        if (class == SEGMENT_UNKNOWN)
            return 0;
        if (class == SEGMENT_STAGE && !piped_from)
            return 0;
        if (class == SEGMENT_READER)
            reads = 1;

        if (!c)
            break;
        piped_from = pipe;
        segment = command + i + width;
        i += width - 1;
    }
    return reads;
}

int toolstyle_is_shell(const char *name)
{
    return name && *name && in_list(SHELL_TOOLS, COUNT(SHELL_TOOLS), name);
}

static const char *shell_command(const char *input_json, const char *arg, char **owned)
{
    *owned = NULL;
    if (!input_json)
        return arg;

    cJSON *input = cJSON_Parse(input_json);
    if (!input)
        return arg;
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItem(input, "command"));
    if (value)
        *owned = strdup(value);
    cJSON_Delete(input);
    return *owned ? *owned : arg;
}

int toolstyle_collapses(const char *name, const char *input_json, const char *arg)
{
    if (!name || !*name)
        return 0;
    if (in_list(READ_TOOLS, COUNT(READ_TOOLS), name))
        return 1;
    if (!in_list(SHELL_TOOLS, COUNT(SHELL_TOOLS), name))
        return 0;

    char       *owned = NULL;
    const char *command = shell_command(input_json, arg, &owned);
    int         reads = toolstyle_shell_reads(command);
    free(owned);
    return reads;
}
