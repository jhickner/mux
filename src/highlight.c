#include "highlight.h"

#include <string.h>

#include "app.h"
#include "ui.h"

#define MAX_DEPTH 4
#define MAX_HEREDOCS 4

enum lang { LANG_NONE, LANG_SHELL, LANG_PYTHON };

struct lex {
    const char    *s;
    size_t         n;
    unsigned char *r;
};

struct heredoc {
    size_t     tag, tag_len;
    int        strip;
    enum lang  lang;
};

static void region(struct lex *L, size_t from, size_t to, enum lang lang, int depth);
static void shell_region(struct lex *L, size_t from, size_t to, int depth);
static void python_region(struct lex *L, size_t from, size_t to);

static void paint(struct lex *L, size_t from, size_t to, enum ui_role role)
{
    if (to > L->n)
        to = L->n;
    for (size_t i = from; i < to; i++)
        L->r[i] = (unsigned char)role;
}

static int blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static int alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static int digit(char c) { return c >= '0' && c <= '9'; }

static int name_char(char c) { return alpha(c) || digit(c) || c == '_'; }

static int in_list(const char *const *list, int n, const char *word)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i], word))
            return 1;
    return 0;
}

/* ---- shell ---------------------------------------------------------- */

static const char *const SHELL_KEYWORDS[] = {
    "if",   "then", "else",  "elif",     "fi",     "for",    "while", "until",
    "do",   "done", "case",  "esac",     "in",     "function", "select",
    "return", "break", "continue", "!",
};

/* Words after which the next word names a variable, not a command. */
static const char *const SHELL_NAMERS[] = {"for", "select"};

static const char *const SHELL_NAMES[] = {"sh", "bash", "zsh", "dash", "ksh"};

static int python_program(const char *word)
{
    if (!strcmp(word, "ipython") || !strcmp(word, "pypy") || !strcmp(word, "pypy3"))
        return 1;
    if (strncmp(word, "python", 6))
        return 0;
    for (const char *p = word + 6; *p; p++)
        if (!digit(*p) && *p != '.')
            return 0;
    return 1;
}

static size_t skip_pair(struct lex *L, size_t i, size_t to, char open, char close)
{
    int depth = 0;
    for (; i < to; i++) {
        char c = L->s[i];
        if (c == '\\' && i + 1 < to) {
            i++;
            continue;
        }
        if (c == open)
            depth++;
        else if (c == close && --depth == 0)
            return i + 1;
    }
    return to;
}

static size_t skip_quote(struct lex *L, size_t i, size_t to)
{
    char quote = L->s[i++];
    for (; i < to; i++) {
        if (quote != '\'' && L->s[i] == '\\' && i + 1 < to) {
            i++;
            continue;
        }
        if (L->s[i] == quote)
            return i + 1;
    }
    return to;
}

static size_t scan_word(struct lex *L, size_t i, size_t to)
{
    const char *s = L->s;
    while (i < to) {
        char c = s[i];
        if (blank(c) || c == '\n' || strchr("|&;<>()", c))
            break;
        if (c == '\\' && i + 1 < to) {
            i += 2;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') {
            i = skip_quote(L, i, to);
            continue;
        }
        if (c == '$' && i + 1 < to && s[i + 1] == '(') {
            i = skip_pair(L, i + 1, to, '(', ')');
            continue;
        }
        if (c == '$' && i + 1 < to && s[i + 1] == '{') {
            i = skip_pair(L, i + 1, to, '{', '}');
            continue;
        }
        i++;
    }
    return i;
}

/* Literal spelling of a word, or 0 when quoting or expansion hides it. */
static int word_text(struct lex *L, size_t from, size_t to, char *out, size_t size)
{
    size_t o = 0;
    for (size_t i = from; i < to; i++) {
        char c = L->s[i];
        if (c == '$' || c == '`' || c == '\'' || c == '"' || c == '\\')
            return 0;
        if (o + 1 >= size)
            return 0;
        out[o++] = c;
    }
    out[o] = '\0';
    return 1;
}

static const char *basename_of(const char *word)
{
    const char *slash = strrchr(word, '/');
    return slash ? slash + 1 : word;
}

static size_t paint_dollar(struct lex *L, size_t i, size_t to, int depth)
{
    const char *s = L->s;
    if (i + 1 >= to)
        return i + 1;

    if (s[i + 1] == '(') {
        size_t end = skip_pair(L, i + 1, to, '(', ')');
        paint(L, i, i + 2, UI_SYN_OP);
        if (end > i + 2) {
            shell_region(L, i + 2, end - 1, depth + 1);
            paint(L, end - 1, end, UI_SYN_OP);
        }
        return end;
    }
    if (s[i + 1] == '{') {
        size_t end = skip_pair(L, i + 1, to, '{', '}');
        paint(L, i, end, UI_SYN_VAR);
        return end;
    }
    size_t j = i + 1;
    if (name_char(s[j]))
        while (j < to && name_char(s[j]))
            j++;
    else if (strchr("@*#?$!-", s[j]))
        j++;
    paint(L, i, j, UI_SYN_VAR);
    return j;
}

/* Paints a word as `base`, then overlays the quoted runs and expansions. */
static void paint_word(struct lex *L, size_t from, size_t to, enum ui_role base, int depth)
{
    const char *s = L->s;
    paint(L, from, to, base);

    size_t i = from;
    while (i < to) {
        char c = s[i];
        if (c == '\\' && i + 1 < to) {
            i += 2;
            continue;
        }
        if (c == '\'') {
            size_t end = skip_quote(L, i, to);
            paint(L, i, end, UI_SYN_STRING);
            i = end;
            continue;
        }
        if (c == '"') {
            size_t end = skip_quote(L, i, to);
            paint(L, i, end, UI_SYN_STRING);
            for (size_t j = i + 1; j < end;) {
                if (s[j] == '\\' && j + 1 < end)
                    j += 2;
                else if (s[j] == '$')
                    j = paint_dollar(L, j, end, depth);
                else
                    j++;
            }
            i = end;
            continue;
        }
        if (c == '`') {
            size_t end = skip_quote(L, i, to);
            paint(L, i, i + 1, UI_SYN_OP);
            if (end > i + 2) {
                shell_region(L, i + 1, end - 1, depth + 1);
                paint(L, end - 1, end, UI_SYN_OP);
            }
            i = end;
            continue;
        }
        if (c == '$') {
            i = paint_dollar(L, i, to, depth);
            continue;
        }
        i++;
    }
}

static int numeric_word(const char *word)
{
    if (*word == '-' || *word == '+')
        word++;
    if (!digit(*word))
        return 0;
    for (const char *p = word; *p; p++)
        if (!digit(*p) && *p != '.')
            return 0;
    return 1;
}

/* An assignment prefix (FOO=bar cmd) or a plain `NAME=` word. */
static int paint_assignment(struct lex *L, size_t from, size_t to, int depth)
{
    size_t i = from;
    if (i >= to || (!alpha(L->s[i]) && L->s[i] != '_'))
        return 0;
    while (i < to && name_char(L->s[i]))
        i++;
    if (i >= to || L->s[i] != '=')
        return 0;

    paint(L, from, i, UI_SYN_VAR);
    paint(L, i, i + 1, UI_SYN_OP);
    paint_word(L, i + 1, to, UI_RESET, depth);
    return 1;
}

/* Paints inline code (python -c '…'), keeping the surrounding quotes visible. */
static void paint_code(struct lex *L, size_t from, size_t to, enum lang lang, int depth)
{
    char quote = from < to ? L->s[from] : 0;
    if ((quote == '\'' || quote == '"') && to - from >= 2 && L->s[to - 1] == quote) {
        paint(L, from, from + 1, UI_SYN_STRING);
        paint(L, to - 1, to, UI_SYN_STRING);
        from++;
        to--;
    }
    region(L, from, to, lang, depth + 1);
}

static size_t heredoc_bodies(struct lex *L, size_t i, size_t to,
                             const struct heredoc *hd, int count, int depth)
{
    const char *s = L->s;
    for (int k = 0; k < count; k++) {
        size_t body = i, end = to, next = to, tag = 0, tag_end = 0;

        for (size_t p = i; p < to;) {
            size_t eol = p;
            while (eol < to && s[eol] != '\n')
                eol++;
            size_t start = p;
            if (hd[k].strip)
                while (start < eol && s[start] == '\t')
                    start++;
            if (eol - start == hd[k].tag_len &&
                !memcmp(s + start, s + hd[k].tag, hd[k].tag_len)) {
                end = p;
                tag = start;
                tag_end = eol;
                next = eol < to ? eol + 1 : to;
                break;
            }
            p = eol < to ? eol + 1 : to;
        }

        region(L, body, end, hd[k].lang, depth + 1);
        if (tag_end)
            paint(L, tag, tag_end, UI_SYN_STRING);
        i = next;
    }
    return i;
}

static void shell_region(struct lex *L, size_t from, size_t to, int depth)
{
    const char *s = L->s;
    if (depth >= MAX_DEPTH)
        return;

    struct heredoc pending[MAX_HEREDOCS];
    int            npending = 0;
    int            cmdpos = 1, naming = 0;
    enum lang      lang = LANG_NONE;
    int            want_code = 0;

    for (size_t i = from; i < to;) {
        char c = s[i];

        if (blank(c)) {
            i++;
            continue;
        }
        if (c == '\n') {
            i++;
            if (npending) {
                i = heredoc_bodies(L, i, to, pending, npending, depth);
                npending = 0;
            }
            cmdpos = 1;
            naming = 0;
            lang = LANG_NONE;
            want_code = 0;
            continue;
        }
        if (c == '#' && (i == from || blank(s[i - 1]) || s[i - 1] == '\n')) {
            size_t j = i;
            while (j < to && s[j] != '\n')
                j++;
            paint(L, i, j, UI_SYN_COMMENT);
            i = j;
            continue;
        }
        if (c == '|' || c == '&' || c == ';') {
            size_t j = i + 1;
            if (j < to && s[j] == c)
                j++;
            paint(L, i, j, UI_SYN_OP);
            i = j;
            cmdpos = 1;
            naming = 0;
            lang = LANG_NONE;
            want_code = 0;
            continue;
        }
        if (c == '(' || c == ')' || c == '{' || c == '}') {
            paint(L, i, i + 1, UI_SYN_OP);
            i++;
            cmdpos = 1;
            naming = 0;
            continue;
        }

        /* Heredoc: `<<TAG`, `<<-TAG`, `<< 'TAG'`. */
        if (c == '<' && i + 1 < to && s[i + 1] == '<' &&
            !(i + 2 < to && (s[i + 2] == '<' || s[i + 2] == '('))) {
            size_t j = i + 2;
            int    strip = 0;
            if (j < to && s[j] == '-') {
                strip = 1;
                j++;
            }
            paint(L, i, j, UI_SYN_OP);
            while (j < to && blank(s[j]))
                j++;
            size_t word_end = scan_word(L, j, to);
            size_t tag = j, tag_end = word_end;
            if (tag < tag_end && (s[tag] == '\'' || s[tag] == '"')) {
                tag++;
                if (tag_end > tag && s[tag_end - 1] == s[j])
                    tag_end--;
            }
            paint(L, j, word_end, UI_SYN_STRING);
            if (npending < MAX_HEREDOCS && tag_end > tag) {
                pending[npending].tag = tag;
                pending[npending].tag_len = tag_end - tag;
                pending[npending].strip = strip;
                pending[npending].lang = lang;
                npending++;
            }
            i = word_end;
            cmdpos = 0;
            continue;
        }
        if (c == '<' || c == '>' ||
            (digit(c) && i + 1 < to && (s[i + 1] == '>' || s[i + 1] == '<') &&
             (i == from || blank(s[i - 1])))) {
            size_t j = digit(c) ? i + 2 : i + 1;
            if (j < to && s[j] == '>')
                j++;
            if (j < to && s[j] == '&') {
                j++;
                while (j < to && (digit(s[j]) || s[j] == '-'))
                    j++;
            }
            paint(L, i, j, UI_SYN_OP);
            i = j;
            continue;
        }

        size_t end = scan_word(L, i, to);
        if (end == i) {
            i++;
            continue;
        }

        char word[256];
        int  literal = word_text(L, i, end, word, sizeof word);
        const char *base = literal ? basename_of(word) : "";

        if (literal && python_program(base))
            lang = LANG_PYTHON;
        else if (literal && in_list(SHELL_NAMES, COUNT(SHELL_NAMES), base))
            lang = LANG_SHELL;

        if (want_code) {
            paint_code(L, i, end, lang, depth);
            want_code = 0;
        } else if (literal && cmdpos && in_list(SHELL_KEYWORDS, COUNT(SHELL_KEYWORDS), word)) {
            paint(L, i, end, UI_SYN_KEYWORD);
            naming = in_list(SHELL_NAMERS, COUNT(SHELL_NAMERS), word);
            cmdpos = strcmp(word, "in") != 0;
            i = end;
            continue;
        } else if (naming) {
            paint_word(L, i, end, UI_SYN_VAR, depth);
            naming = 0;
        } else if (cmdpos && paint_assignment(L, i, end, depth)) {
            i = end;
            continue;
        } else if (cmdpos) {
            paint_word(L, i, end, UI_SYN_CMD, depth);
            cmdpos = 0;
        } else if (s[i] == '-' && end - i > 1) {
            paint_word(L, i, end, UI_SYN_FLAG, depth);
            if (lang != LANG_NONE && literal && !strcmp(word, "-c"))
                want_code = 1;
        } else if (literal && numeric_word(word)) {
            paint(L, i, end, UI_SYN_NUMBER);
        } else {
            paint_word(L, i, end, UI_RESET, depth);
        }
        i = end;
    }
}

/* ---- python --------------------------------------------------------- */

static const char *const PY_KEYWORDS[] = {
    "and",    "as",     "assert", "async",  "await",    "break",  "class",
    "continue", "def",  "del",    "elif",   "else",     "except", "finally",
    "for",    "from",   "global", "if",     "import",   "in",     "is",
    "lambda", "nonlocal", "not",  "or",     "pass",     "raise",  "return",
    "try",    "while",  "with",   "yield",  "True",     "False",  "None",
    "self",
};

static size_t py_string(struct lex *L, size_t i, size_t to)
{
    const char *s = L->s;
    size_t      start = i;

    while (i < to && alpha(s[i]))
        i++;
    if (i >= to || (s[i] != '\'' && s[i] != '"') || i - start > 2)
        return start;

    char   quote = s[i];
    int    raw = 0;
    for (size_t p = start; p < i; p++)
        if (s[p] == 'r' || s[p] == 'R')
            raw = 1;

    size_t run = i + 3 <= to && s[i + 1] == quote && s[i + 2] == quote ? 3 : 1;
    i += run;
    while (i < to) {
        if (!raw && s[i] == '\\' && i + 1 < to) {
            i += 2;
            continue;
        }
        if (s[i] == quote && (run == 1 || (i + 3 <= to && s[i + 1] == quote && s[i + 2] == quote))) {
            i += run;
            break;
        }
        if (run == 1 && s[i] == '\n')
            break;
        i++;
    }
    paint(L, start, i, UI_SYN_STRING);
    return i;
}

static void python_region(struct lex *L, size_t from, size_t to)
{
    const char *s = L->s;
    int         defining = 0;

    for (size_t i = from; i < to;) {
        char c = s[i];

        if (c == '#') {
            size_t j = i;
            while (j < to && s[j] != '\n')
                j++;
            paint(L, i, j, UI_SYN_COMMENT);
            i = j;
            continue;
        }
        if (c == '\n') {
            defining = 0;
            i++;
            continue;
        }
        if (c == '\'' || c == '"' || alpha(c)) {
            size_t end = py_string(L, i, to);
            if (end > i) {
                i = end;
                continue;
            }
        }
        if (c == '@' && i + 1 < to && (alpha(s[i + 1]) || s[i + 1] == '_')) {
            size_t j = i + 1;
            while (j < to && (name_char(s[j]) || s[j] == '.'))
                j++;
            paint(L, i, j, UI_SYN_VAR);
            i = j;
            continue;
        }
        if (digit(c) && (i == from || !name_char(s[i - 1]))) {
            size_t j = i;
            while (j < to && (name_char(s[j]) || s[j] == '.'))
                j++;
            paint(L, i, j, UI_SYN_NUMBER);
            i = j;
            continue;
        }
        if (alpha(c) || c == '_') {
            size_t j = i;
            while (j < to && name_char(s[j]))
                j++;

            char word[64];
            size_t len = j - i < sizeof word - 1 ? j - i : sizeof word - 1;
            memcpy(word, s + i, len);
            word[len] = '\0';

            size_t call = j;
            while (call < to && blank(s[call]))
                call++;

            if (in_list(PY_KEYWORDS, COUNT(PY_KEYWORDS), word)) {
                paint(L, i, j, UI_SYN_KEYWORD);
                defining = !strcmp(word, "def") || !strcmp(word, "class");
            } else if (defining) {
                paint(L, i, j, UI_SYN_CMD);
                defining = 0;
            } else if (call < to && s[call] == '(' && (i == from || s[i - 1] != '.')) {
                paint(L, i, j, UI_SYN_CMD);
            }
            i = j;
            continue;
        }
        i++;
    }
}

static void region(struct lex *L, size_t from, size_t to, enum lang lang, int depth)
{
    if (from >= to)
        return;
    if (lang == LANG_PYTHON)
        python_region(L, from, to);
    else if (lang == LANG_SHELL)
        shell_region(L, from, to, depth);
}

void highlight_shell(const char *text, size_t len, unsigned char *roles)
{
    if (!text || !roles || !len)
        return;
    memset(roles, (unsigned char)UI_RESET, len);

    struct lex lex = {text, len, roles};
    shell_region(&lex, 0, len, 0);
}
