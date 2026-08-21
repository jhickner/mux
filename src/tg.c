// The Telegram front end. Two threads: the poller does the chat's network I/O
// and drops what arrives into an inbox; everything else — dispatching a line,
// running a turn, rendering what the agent does — happens on the main thread,
// which is the only one that may touch the session.
//
// With a terminal, this is a mirror: the same session answers at the prompt and
// on the phone. Without one it is the whole front end.

#include "tg.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TELEGRAM_IMPLEMENTATION
#include "vendor/telegram.h"
#define WHISPER_IMPLEMENTATION
#include "vendor/whisper.h"
#define HTTPD_IMPLEMENTATION
#include "vendor/httpd.h"
#include "vendor/mdv2.h"

#include "app.h"
#include "bash.h"
#include "chattabs.h"
#include "cmd.h"
#include "frontend.h"
#include "gitinfo.h"
#include "reminders.h"
#include "handoff.h"
#include "restart.h"
#include "session.h"
#include "sessionlist.h"
#include "status.h"
#include "text.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"
#include "workspace.h"

#define TG_LIMIT    4000        // Telegram caps a message at 4096 characters
#define MAX_ATTACH  8
#define INBOX_MAX   32

enum { MIRROR_OFF = 0, MIRROR_REMOTE = 1, MIRROR_ALL = 2 };

static struct session *sess;
static tg_client      *rx;              // poller thread's client
static tg_client      *tx;              // main thread's client
static long            chat_id;
static int             headless_mode;
static int             mirror = MIRROR_ALL;
static int             running;
static volatile int    quit_wanted;
static volatile int    stop_wanted;     // "stop": abandon the turn in flight
static volatile int    poller_stop;
static pthread_t       poller;
static int             wake[2] = {-1, -1};
static char            label[96];       // "telegram @bot", for the hud
static whisper_config  voice;           // where the voice-note transcriber lives
static int             voice_set;
static int             poll_seconds = 30;
static char           *last_said;       // last assistant text already relayed
static char            last_log[240];   // the client's last complaint, for /tg
static int             log_repeats;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static int             from_chat;       // the turn now running came from the chat

// ---- config -------------------------------------------------------------

// ~/.config/mux/telegram, "key = value" per line. The token is a secret and
// comes from the environment only.
#define CFG_MAX 32
static struct { char key[64]; char val[512]; } cfg[CFG_MAX];
static int cfg_count = -1;

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

static void cfg_load(void)
{
    cfg_count = 0;
    char path[4200];
    if (!path_config_file(path, sizeof path, "telegram"))
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[700];
    while (fgets(line, sizeof line, f) && cfg_count < CFG_MAX) {
        char *s = trim(line);
        if (!*s || *s == '#')
            continue;
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *k = trim(s), *v = trim(eq + 1);
        if (!*k)
            continue;
        snprintf(cfg[cfg_count].key, sizeof cfg[0].key, "%s", k);
        snprintf(cfg[cfg_count].val, sizeof cfg[0].val, "%s", v);
        cfg_count++;
    }
    fclose(f);
}

static const char *cfg_get(const char *key, const char *dflt)
{
    if (cfg_count < 0)
        cfg_load();
    for (int i = 0; i < cfg_count; i++)
        if (!strcmp(cfg[i].key, key) && cfg[i].val[0])
            return cfg[i].val;
    return dflt;
}

static long cfg_get_long(const char *key, long dflt)
{
    const char *v = cfg_get(key, NULL);
    return v ? strtol(v, NULL, 10) : dflt;
}

// Small pieces of state the bridge keeps between runs, one per file, under
// ~/.config/mux/tg-<name>: the update offset, the artifact token.
static int state_path(const char *name, char *out, size_t size)
{
    char leaf[64];
    snprintf(leaf, sizeof leaf, "tg-%s", name);
    return path_config_file(out, size, leaf);
}

static char *state_read(const char *name)
{
    char path[4200];
    if (!state_path(name, path, sizeof path))
        return NULL;
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    char buf[512] = {0};
    char *r = fgets(buf, sizeof buf, f);
    fclose(f);
    if (!r)
        return NULL;
    text_chomp(buf);
    return buf[0] ? strdup(buf) : NULL;
}

static void state_write(const char *name, const char *value)
{
    char path[4200];
    if (!state_path(name, path, sizeof path))
        return;
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "%s\n", value ? value : "");
    fclose(f);
}

// ---- inbox --------------------------------------------------------------

// What the poller has taken from the chat and the main thread has not run yet.
struct inbox_item {
    char *text;
    int   quiet;    // the daemon asked, not the user: say nothing if there is
                    // nothing to say
    int   tap;      // an inline button, not something typed: `text` is the
                    // menu payload behind it, not a line for the agent
};

static struct inbox_item inbox[INBOX_MAX];
static int inbox_head, inbox_count;
static pthread_mutex_t inbox_lock = PTHREAD_MUTEX_INITIALIZER;

static void wake_up(void)
{
    if (wake[1] >= 0) {
        char b = 1;
        ssize_t ignored = write(wake[1], &b, 1);
        (void)ignored;
    }
}

static void wake_drain(void)
{
    char buf[64];
    while (wake[0] >= 0 && read(wake[0], buf, sizeof buf) > 0)
        ;
}

static int inbox_push_kind(char *text, int quiet, int tap)
{
    if (!text)
        return 0;
    pthread_mutex_lock(&inbox_lock);
    int ok = inbox_count < INBOX_MAX;
    if (ok) {
        inbox[(inbox_head + inbox_count) % INBOX_MAX].text = text;
        inbox[(inbox_head + inbox_count) % INBOX_MAX].quiet = quiet;
        inbox[(inbox_head + inbox_count) % INBOX_MAX].tap = tap;
        inbox_count++;
    }
    pthread_mutex_unlock(&inbox_lock);
    if (ok)
        wake_up();
    else
        free(text);
    return ok;
}

static int inbox_push(char *text, int quiet)
{
    return inbox_push_kind(text, quiet, 0);
}

static char *inbox_take(int *quiet, int *tap)
{
    pthread_mutex_lock(&inbox_lock);
    char *text = NULL;
    if (inbox_count > 0) {
        text = inbox[inbox_head].text;
        if (quiet)
            *quiet = inbox[inbox_head].quiet;
        if (tap)
            *tap = inbox[inbox_head].tap;
        inbox_head = (inbox_head + 1) % INBOX_MAX;
        inbox_count--;
    }
    int remaining = inbox_count;
    pthread_mutex_unlock(&inbox_lock);
    if (text) {
        wake_drain();
        if (remaining)
            wake_up();          // the next one still has to wake the prompt
    }
    return text;
}

int tg_pending(void)
{
    pthread_mutex_lock(&inbox_lock);
    int n = inbox_count;
    pthread_mutex_unlock(&inbox_lock);
    return n;
}

int tg_fds(int *out, int max)
{
    if (!running || max < 1 || wake[0] < 0)
        return 0;
    out[0] = wake[0];
    return 1;
}

// ---- text helpers -------------------------------------------------------

// Copy at most size-1 bytes, marking truncation with an ellipsis. A cut never
// lands inside a UTF-8 sequence: Telegram rejects invalid UTF-8.
static void copy_trunc(char *dst, size_t size, const char *src)
{
    size_t i = 0;
    for (; src && src[i] && i < size - 1; i++)
        dst[i] = src[i];
    if (src && src[i]) {
        for (size_t j = i; j > 0; j--) {
            unsigned char c = (unsigned char)dst[j - 1];
            if ((c & 0xC0) == 0x80)
                continue;                          // continuation byte
            size_t need = (c & 0x80) == 0    ? 1 : (c & 0xE0) == 0xC0 ? 2
                        : (c & 0xF0) == 0xE0 ? 3 : 4;
            if (j - 1 + need > i)
                i = j - 1;                         // the sequence was cut
            break;
        }
        if (i + 4 <= size) {
            memcpy(dst + i, "...", 3);
            i += 3;
        }
    }
    dst[i] = '\0';
}

// As copy_trunc, but collapsing newlines so a tool preview stays on one line.
static void one_line(char *dst, size_t size, const char *src)
{
    copy_trunc(dst, size, src);
    for (size_t i = 0; dst[i]; i++)
        if (dst[i] == '\n' || dst[i] == '\r' || dst[i] == '\t')
            dst[i] = ' ';
}

// The terminal's own output, stripped of the escapes that styled it.
static char *strip_ansi(char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s), w = 0;
    for (size_t i = 0; i < n;) {
        enum ui_esc_kind kind;
        size_t end = ui_esc_span(s, n, i, &kind);
        if (kind == UI_ESC_TEXT)
            for (size_t k = i; k < end; k++)
                if (s[k] != '\r')
                    s[w++] = s[k];
        i = end;
    }
    s[w] = '\0';
    return s;
}

// ---- sending ------------------------------------------------------------

// The agent writes markdown; render it as MarkdownV2, split so no message
// exceeds Telegram's cap and none is cut mid-entity.
static void send_markdown(const char *text)
{
    if (!tx || !chat_id)
        return;
    if (!text || !*text) {
        tg_send_message(tx, chat_id, "(done)");
        return;
    }
    size_t n = 0;
    char **msgs = mdv2_messages(text, TG_LIMIT, &n);
    if (!msgs) {
        tg_send_message(tx, chat_id, text);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (msgs[i] && *msgs[i])
            tg_send_message_md(tx, chat_id, msgs[i]);
        free(msgs[i]);
    }
    free(msgs);
}

// Command output is column-aligned, so it goes as a code block: Telegram's body
// font is proportional and would collapse the columns.
static void send_pre(const char *text)
{
    if (!tx || !chat_id || !text)
        return;
    while (*text == '\n')
        text++;
    if (!*text)
        return;
    mdv2_buf b = {0};
    mdv2_puts(&b, "```\n");
    mdv2_esc_code(&b, text, strlen(text));
    mdv2_puts(&b, "\n```");
    if (b.p && b.n < TG_LIMIT)
        tg_send_message_md(tx, chat_id, b.p);
    else
        send_markdown(text);
    free(b.p);
}

// One italic line: the bridge talking, not the agent.
static void send_note(const char *text)
{
    if (!tx || !chat_id || !text || !*text)
        return;
    mdv2_buf b = {0};
    mdv2_putc(&b, '_');
    mdv2_esc(&b, text, strlen(text));
    mdv2_putc(&b, '_');
    if (b.p)
        tg_send_message_md(tx, chat_id, b.p);
    free(b.p);
}

__attribute__((format(printf, 1, 2)))
static void send_notef(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    send_note(line);
}

// ---- menus --------------------------------------------------------------

// A menu is a message with buttons under it. The chat keeps every message it
// has ever been sent, so a tap can arrive from a menu scrolled back to weeks
// later: each one carries a serial, only the newest is live, and a tap on any
// older one is answered with a note instead of acted on. Once a live menu has
// been used its buttons are edited away, which is both the receipt and what
// stops it being tapped twice.
//
// Telegram caps a button's payload at 64 bytes, so what travels is the serial
// and a row number; what the row means is kept here.

#define MENU_MAX 12

static struct {
    long serial;                    // 0 when no menu is live
    long message_id;
    char kind[16];                  // whose menu it is, for the handler
    char payload[MENU_MAX][200];    // what each row means, in that kind's terms
    char label[MENU_MAX][80];
    int  count;
} menu;
static long menu_serial;

static void menu_begin(const char *kind)
{
    menu.count = 0;
    menu.serial = 0;
    menu.message_id = 0;
    snprintf(menu.kind, sizeof menu.kind, "%s", kind);
}

static void menu_add(const char *label, const char *payload)
{
    if (menu.count >= MENU_MAX)
        return;
    snprintf(menu.label[menu.count], sizeof menu.label[0], "%s", label);
    snprintf(menu.payload[menu.count], sizeof menu.payload[0], "%s",
             payload ? payload : "");
    menu.count++;
}

// Sends what has been added as a keyboard under `title`. Without buttons —
// nothing was added, or the send failed — the caller still gets its text out,
// so a menu is never the only way to see something.
static void menu_send(const char *title, int per_row)
{
    if (!tx || !chat_id || !menu.count) {
        menu.serial = 0;
        if (title && *title)
            send_note(title);
        return;
    }

    tg_button buttons[MENU_MAX];
    char data[MENU_MAX][32];
    long serial = ++menu_serial;
    for (int i = 0; i < menu.count; i++) {
        snprintf(data[i], sizeof data[0], "%ld:%d", serial, i);
        buttons[i].label = menu.label[i];
        buttons[i].data = data[i];
    }

    long id = tg_send_keyboard(tx, chat_id, title, 0, buttons, menu.count, per_row);
    if (id < 0) {
        menu.serial = 0;
        send_note("could not put the menu up");
        return;
    }
    menu.serial = serial;
    menu.message_id = id;
}

// A tap has come back. Fills `kind` and `payload` with what the row meant, or
// says so in the chat and returns 0 when the menu it came from is not the live
// one.
// What the last tap picked, waiting to be written back over its own menu.
static struct {
    long message_id;
    char label[80];
} receipt;

static int menu_take(const char *tapped, char *kind, size_t kind_size,
                     char *payload, size_t payload_size)
{
    long serial = 0;
    int  row = -1;
    if (!tapped || sscanf(tapped, "%ld:%d", &serial, &row) != 2)
        return 0;
    if (!menu.serial || serial != menu.serial || row < 0 || row >= menu.count) {
        send_note("that menu is out of date");
        return 0;
    }
    snprintf(kind, kind_size, "%s", menu.kind);
    snprintf(payload, payload_size, "%s", menu.payload[row]);

    // Spent: the buttons go, and the message says what was picked in their
    // place. That is a round trip, and a tap is resolved wherever it happens
    // to arrive — inside the prompt's read, with a terminal attached — so it
    // is left for menu_flush() to do somewhere it can afford to wait.
    if (menu.message_id) {
        receipt.message_id = menu.message_id;
        snprintf(receipt.label, sizeof receipt.label, "%s", menu.label[row]);
    }
    menu.serial = 0;
    menu.count = 0;
    return 1;
}

// Retires the menu the last tap came from.
static void menu_flush(void)
{
    if (!receipt.message_id || !tx)
        return;
    long id = receipt.message_id;
    receipt.message_id = 0;
    tg_edit_message(tx, chat_id, id, receipt.label, 0, NULL, 0, 0);
}

// A local image the answer points at goes as a photo: a path on this machine
// means nothing on the phone. The link stays in the text, which is what says
// where it came from.
static void send_images(const char *text)
{
    for (const char *p = text; (p = strstr(p, "![")) != NULL;) {
        const char *open = strstr(p, "](");
        if (!open)
            return;
        open += 2;
        const char *close = strchr(open, ')');
        p = open;
        if (!close || *open != '/' || close - open > 1023)
            continue;
        char path[1024];
        snprintf(path, sizeof path, "%.*s", (int)(close - open), open);
        if (access(path, R_OK) == 0)
            tg_send_photo(tx, chat_id, path, NULL);
        p = close;
    }
}

// ---- the tool trace -----------------------------------------------------

// Drop the project dir prefix so a path reads as it would in the editor.
static const char *short_path(const char *p)
{
    const char *cwd = sess ? session_cwd(sess) : NULL;
    size_t n = cwd ? strlen(cwd) : 0;
    if (n && !strncmp(p, cwd, n) && p[n] == '/')
        return p + n + 1;
    return p;
}

// "mcp__gmail__send_email" and "Bash" both want their last readable segment.
static void tool_label(char *dst, size_t size, const char *name)
{
    if (!strncmp(name, "mcp__", 5)) {
        const char *sep = name, *last = name + 5;
        while ((sep = strstr(sep, "__")) != NULL) {
            last = sep + 2;
            sep += 2;
        }
        name = last;
    }
    size_t i = 0;
    for (; name[i] && i < size - 1; i++)
        dst[i] = (char)tolower((unsigned char)name[i]);
    dst[i] = '\0';
}

// The one argument worth showing for a tool call: a path, a command, a query.
static const char *tool_arg(cJSON *in)
{
    static const char *const keys[] = {
        "file_path", "notebook_path", "command", "pattern", "query", "url",
        "skill", "description", "prompt", "path", "message", NULL
    };
    for (int i = 0; keys[i]; i++) {
        const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(in, keys[i]));
        if (v && *v)
            return v;
    }
    return NULL;
}

// "read" + "src/main.c", "bash" + "make -j8". The argument falls back to the
// compact JSON, and is empty when the tool takes nothing worth showing.
static void tool_line(char *label, size_t ln, char *arg, size_t an,
                      const backend_event *ev)
{
    tool_label(label, ln, ev->name ? ev->name : "tool");
    cJSON *in = ev->input_json ? cJSON_Parse(ev->input_json) : NULL;
    const char *v = in ? tool_arg(in) : ev->arg;
    if (v && (!strcmp(label, "read") || !strcmp(label, "edit") ||
              !strcmp(label, "write") || !strcmp(label, "notebookedit")))
        v = short_path(v);
    copy_trunc(arg, an, v ? v : (ev->input_json ? ev->input_json : ""));
    cJSON_Delete(in);
}

#define DIFF_MAX_LINES 40   // a long edit is cut rather than split in two

// Lines of `s` prefixed with `sign` for a ```diff block. *left is the line
// budget, decremented as lines are written; exhausting it emits an ellipsis.
static void put_diff_lines(mdv2_buf *b, const char *s, char sign, int *left)
{
    while (s && *s) {
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s);
        if (*left <= 0) {
            mdv2_puts(b, "...\n");
            return;
        }
        char tmp[512], line[220];
        size_t k = len < sizeof tmp - 1 ? len : sizeof tmp - 1;
        memcpy(tmp, s, k);
        tmp[k] = '\0';
        copy_trunc(line, sizeof line, tmp);
        mdv2_putc(b, sign);
        mdv2_esc_code(b, line, strlen(line));
        mdv2_putc(b, '\n');
        (*left)--;
        if (!nl)
            return;
        s = nl + 1;
    }
}

// An edit reads best as a diff: Telegram colours -/+ lines inside a ```diff
// block. A write has no prior text, so its whole body shows as added. Returns 0
// when the call carries no diffable text, leaving the caller to fall back.
static int send_edit_diff(const char *label, const char *path, cJSON *in)
{
    const char *old = cJSON_GetStringValue(cJSON_GetObjectItem(in, "old_string"));
    const char *nw = strcmp(label, "write")
        ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "new_string"))
        : cJSON_GetStringValue(cJSON_GetObjectItem(in, "content"));
    if ((!old || !*old) && (!nw || !*nw))
        return 0;

    mdv2_buf b = {0};
    mdv2_putc(&b, '_');
    mdv2_esc(&b, label, strlen(label));
    mdv2_putc(&b, '_');
    if (path && *path) {
        mdv2_puts(&b, " `");
        mdv2_esc_code(&b, path, strlen(path));
        mdv2_putc(&b, '`');
    }
    mdv2_puts(&b, "\n```diff\n");
    int lo = (old && *old) ? DIFF_MAX_LINES / 2 : 0, ln = DIFF_MAX_LINES - lo;
    put_diff_lines(&b, old, '-', &lo);
    put_diff_lines(&b, nw, '+', &ln);
    mdv2_puts(&b, "```");
    int ok = b.p && b.n < TG_LIMIT;
    if (ok)
        tg_send_message_md(tx, chat_id, b.p);
    free(b.p);
    return ok;
}

// Built by hand rather than converted: the argument is a shell command or a
// path and must never be read as markdown.
static void send_tool_line(const backend_event *ev)
{
    char label[48], raw[600];
    tool_line(label, sizeof label, raw, sizeof raw, ev);
    mdv2_buf b = {0};
    if (*raw && !strcmp(label, "bash")) {
        // No italic label: Telegram already heads the block with "bash", and a
        // command keeps its own line breaks.
        mdv2_puts(&b, "```bash\n");
        mdv2_esc_code(&b, raw, strlen(raw));
        mdv2_puts(&b, "\n```");
        if (b.p)
            tg_send_message_md(tx, chat_id, b.p);
        free(b.p);
        return;
    }
    if (!strcmp(label, "edit") || !strcmp(label, "write") ||
        !strcmp(label, "notebookedit")) {
        cJSON *in = ev->input_json ? cJSON_Parse(ev->input_json) : NULL;
        int sent = in ? send_edit_diff(label, raw, in) : 0;
        cJSON_Delete(in);
        if (sent) {
            free(b.p);
            return;
        }
    }
    mdv2_putc(&b, '_');
    mdv2_esc(&b, label, strlen(label));
    mdv2_putc(&b, '_');
    if (*raw) {
        char one[180];
        one_line(one, sizeof one, raw);
        mdv2_puts(&b, " `");
        mdv2_esc_code(&b, one, strlen(one));
        mdv2_putc(&b, '`');
    }
    if (b.p)
        tg_send_message_md(tx, chat_id, b.p);
    free(b.p);
}

// ---- subagents ----------------------------------------------------------

// Work the agent handed to a subagent. A background subagent outlives the turn
// that launched it, so the turn ending is not the work ending: this is what
// /agents reports and what the idle pump is watching for.
#define SUBAGENT_MAX 16

struct subagent {
    char   id[40];
    char   desc[140];
    char   type[40];
    char   status[24];      // "running", "completed", "failed", ...
    char   latest[240];     // what it is doing now, then what it ended on
    time_t started, ended;
    int    repeats;         // times it ended again after already ending
};

static struct subagent agents[SUBAGENT_MAX];
static int agent_count;
static int task_events;     // the backend reports a task life cycle
static int repeat_task;     // an already-finished task re-notified
static int nudge_queued;

// "launched" counts as done for the running tally: a backend that never reports
// completion must not leave the user reading a count that only grows.
static int subagent_done(const struct subagent *a)
{
    return strcmp(a->status, "running") != 0 && strcmp(a->status, "pending") != 0;
}

static struct subagent *subagent_find(const char *id)
{
    for (int i = 0; i < agent_count; i++)
        if (!strcmp(agents[i].id, id))
            return &agents[i];
    return NULL;
}

// A new task takes a free slot, or the oldest finished one. Nothing is dropped
// while it is still running: those are what the user is waiting on.
static struct subagent *subagent_add(const char *id)
{
    if (agent_count < SUBAGENT_MAX) {
        struct subagent *a = &agents[agent_count++];
        memset(a, 0, sizeof *a);
        snprintf(a->id, sizeof a->id, "%s", id);
        return a;
    }
    for (int i = 0; i < agent_count; i++) {
        if (!subagent_done(&agents[i]))
            continue;
        memmove(&agents[i], &agents[i + 1],
                (size_t)(agent_count - i - 1) * sizeof agents[0]);
        struct subagent *a = &agents[agent_count - 1];
        memset(a, 0, sizeof *a);
        snprintf(a->id, sizeof a->id, "%s", id);
        return a;
    }
    return NULL;
}

static int subagents_running(void)
{
    int n = 0;
    for (int i = 0; i < agent_count; i++)
        if (!subagent_done(&agents[i]))
            n++;
    return n;
}

// "4m12s", the only resolution that matters for a turn measured in minutes.
static void human_secs(char *out, size_t size, long secs)
{
    if (secs < 60)
        snprintf(out, size, "%lds", secs);
    else if (secs < 3600)
        snprintf(out, size, "%ldm%02lds", secs / 60, secs % 60);
    else
        snprintf(out, size, "%ldh%02ldm", secs / 3600, (secs % 3600) / 60);
}

// Fold one task event into the registry. Returns the entry so the caller can
// announce a change; NULL when the event says nothing new.
static struct subagent *subagent_note(const backend_event *ev)
{
    if (!ev->id || !*ev->id)
        return NULL;
    struct subagent *a = subagent_find(ev->id);
    int fresh = 0;
    if (!a) {
        if (!(a = subagent_add(ev->id)))
            return NULL;
        a->started = time(NULL);
        snprintf(a->status, sizeof a->status, "running");
        fresh = 1;
    }
    if (ev->arg && *ev->arg)
        snprintf(a->type, sizeof a->type, "%s", ev->arg);
    if (ev->text && *ev->text) {
        // The launch carries the description, and every event after it carries
        // either what the task is doing now or the summary it ended on — both
        // read as "latest", and neither may overwrite what it was asked to do.
        if (!a->desc[0])
            copy_trunc(a->desc, sizeof a->desc, ev->text);
        else
            copy_trunc(a->latest, sizeof a->latest, ev->text);
    }
    int changed = fresh;
    // The CLI re-announces a task at shutdown, so a finished one is never put
    // back to running: only a status that ends it is taken after it has ended.
    if (ev->name && *ev->name && strcmp(a->status, ev->name) &&
        !(subagent_done(a) && !strcmp(ev->name, "running"))) {
        snprintf(a->status, sizeof a->status, "%s", ev->name);
        changed = 1;
    }
    if (subagent_done(a) && !a->ended)
        a->ended = time(NULL);
    return changed ? a : NULL;
}

// Backends other than claude report no task life cycle: the only evidence a
// subagent exists is the call that spawned it, and nothing announces its end.
// Those are recorded as launches — what was handed off, and when — and never
// pretended to be finished or still running.
static int is_spawn_tool(const char *name)
{
    static const char *const spawn[] = {
        "task", "agent", "spawn_subagent", "spawn_agent", "workflow", NULL
    };
    char lower[64];
    tool_label(lower, sizeof lower, name);      // also strips mcp__ prefixes
    for (int i = 0; spawn[i]; i++)
        if (!strcmp(lower, spawn[i]))
            return 1;
    return 0;
}

static struct subagent *subagent_note_launch(const backend_event *ev)
{
    static int seq;
    if (!ev->name || !is_spawn_tool(ev->name))
        return NULL;
    char id[40];
    snprintf(id, sizeof id, "%s-%d", sess ? session_backend(sess) : "agent", ++seq);
    struct subagent *a = subagent_add(id);
    if (!a)
        return NULL;
    a->started = time(NULL);
    snprintf(a->status, sizeof a->status, "launched");
    cJSON *in = ev->input_json ? cJSON_Parse(ev->input_json) : NULL;
    const char *d = in ? tool_arg(in) : NULL;
    copy_trunc(a->desc, sizeof a->desc, d ? d : ev->name);
    cJSON_Delete(in);
    return a;
}

static void send_subagent_line(const struct subagent *a)
{
    char took[32] = "";
    if (subagent_done(a))
        human_secs(took, sizeof took, (long)(a->ended - a->started));
    send_notef("agent %s: %s%s%s", a->status, a->desc[0] ? a->desc : a->id,
               took[0] ? " in " : "", took);
}

// Append to a fixed buffer, clamping at its end: snprintf reports what it would
// have written, which would run the offset past the buffer on a long line.
__attribute__((format(printf, 4, 5)))
static void appendf(char *buf, size_t size, size_t *n, const char *fmt, ...)
{
    if (*n + 1 >= size)
        return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *n, size - *n, fmt, ap);
    va_end(ap);
    if (w < 0)
        return;
    *n = (size_t)w < size - *n ? *n + (size_t)w : size - 1;
}

static void send_agents(void)
{
    if (!agent_count) {
        send_note("no background agents this session");
        return;
    }
    char msg[2000];
    size_t n = 0;
    time_t now = time(NULL);
    for (int i = 0; i < agent_count; i++) {
        struct subagent *a = &agents[i];
        char took[32], desc[160];
        human_secs(took, sizeof took, (long)((a->ended ? a->ended : now) - a->started));
        one_line(desc, sizeof desc, a->desc[0] ? a->desc : a->id);
        appendf(msg, sizeof msg, &n, "%-9s %-6s %s%s%s\n", a->status, took, desc,
                a->type[0] ? "  @" : "", a->type);
        if (a->latest[0]) {
            char one[160];
            one_line(one, sizeof one, a->latest);
            appendf(msg, sizeof msg, &n, "          %s\n", one);
        }
    }
    appendf(msg, sizeof msg, &n, "\n%d running", subagents_running());
    if (!task_events)
        appendf(msg, sizeof msg, &n,
                "\n%s reports no subagent life cycle, so these are the spawn\n"
                "calls seen: what ran, not how it ended.",
                sess ? session_backend(sess) : "this backend");
    send_pre(msg);
}

// Backends other than claude stream a finished subagent's work but will not say
// anything about it unprompted, so the bridge asks. Queued as an ordinary line:
// it waits behind whatever the user has already sent.
static void nudge_for_subagents(void)
{
    if (nudge_queued || !chat_id)
        return;
    char *text = strdup(
        "Background work you started has just finished. Collect its output and "
        "report to the user now: what it was, what it found or changed, and "
        "anything that needs them. This message is from the daemon, not from "
        "them — your reply goes straight to the chat. If nothing has actually "
        "come back yet, or the result needs no telling, reply with nothing at "
        "all rather than filler.");
    if (text && inbox_push(text, 1))
        nudge_queued = 1;
}

// ---- the observer -------------------------------------------------------

// Whether what the agent is doing right now belongs in the chat.
static int mirroring(void)
{
    if (!running || !chat_id)
        return 0;
    if (mirror == MIRROR_OFF)
        return 0;
    return mirror == MIRROR_ALL || from_chat;
}

// Telegram expires the typing indicator after about five seconds. Refreshing it
// is an HTTP round trip, so it is refreshed on that clock and no faster.
static void typing(void)
{
    static time_t last;
    time_t now = time(NULL);
    if (now - last < 4)
        return;
    last = now;
    tg_send_chat_action(tx, chat_id, "typing");
}

static void on_event(void *ud, const backend_event *ev)
{
    (void)ud;

    if (ev->kind == BACKEND_EV_TASK) {
        task_events = 1;
        // A subagent woken again after finishing — by a background command it
        // left running, or by anything else that resumes it — ends a second
        // time, and the CLI notifies again. That is legitimate: the late result
        // may be the news. But an agent that keeps re-ending is a loop, and
        // every wake costs a turn, so the first repeat is relayed and the rest
        // are dropped.
        struct subagent *prev = subagent_find(ev->id ? ev->id : "");
        int was_done = prev && subagent_done(prev);
        struct subagent *a = subagent_note(ev);
        if (was_done && !a && prev->repeats++ >= 1)
            repeat_task = 1;
        if (a && mirroring())
            send_subagent_line(a);
        return;
    }

    if (!mirroring())
        return;

    if (repeat_task && !from_chat) {
        // Nobody asked, and the only news is a task that already reported. The
        // model still answers the wake; that answer is the duplicate.
        if (ev->kind == BACKEND_EV_ASSISTANT)
            repeat_task = 0;
        return;
    }

    switch (ev->kind) {
    case BACKEND_EV_TOOL:
        if (!task_events) {
            struct subagent *a = subagent_note_launch(ev);
            if (a) {
                send_subagent_line(a);
                break;
            }
        }
        send_tool_line(ev);
        break;

    case BACKEND_EV_ASSISTANT:
        if (!ev->text || !*ev->text)
            break;
        send_markdown(ev->text);
        send_images(ev->text);
        free(last_said);
        last_said = strdup(ev->text);
        break;

    case BACKEND_EV_WARNING:
        if (ev->text && *ev->text)
            send_note(ev->text);
        break;

    default:
        break;
    }
    typing();
}

// Telegram expires the typing indicator after about five seconds, and a turn
// that is only thinking sends nothing else.
static int on_abort(void *ud)
{
    const struct session *s = ud;
    if (mirroring())
        typing();
    // "stop" means the turn the chat is watching. A tab working in the
    // background is not what was being complained about.
    if (stop_wanted && (!s || s == sess)) {
        stop_wanted = 0;
        return 1;
    }
    return 0;
}

// The client's own diagnostics, which come off the polling thread. With a
// terminal they are kept rather than printed: writing there from another thread
// lands in the middle of whatever is drawn. /tg reports the last one.
static void on_log(const char *msg)
{
    if (headless_mode) {
        fprintf(stderr, "telegram: %s\n", msg);
        return;
    }
    pthread_mutex_lock(&log_lock);
    if (!strcmp(last_log, msg)) {
        log_repeats++;
    } else {
        snprintf(last_log, sizeof last_log, "%s", msg);
        log_repeats = 0;
    }
    pthread_mutex_unlock(&log_lock);
}

// Something worth knowing as the bridge comes up. With no terminal that is the
// log; with one it has to go through the ui, or it lands on the screen as raw
// bytes the viewport does not know about and the hud paints over.
__attribute__((format(printf, 1, 2)))
static void note_up(const char *fmt, ...)
{
    char line[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    if (headless_mode) {
        fprintf(stderr, APP_NAME ": %s\n", line);
        return;
    }
    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    ui_note("%s", line);
    viewport_item_end();
    ui_flush();
}

// ---- artifacts ----------------------------------------------------------

static httpd *server;
static char   art_dir[4096];
static char   art_base[300];    // link prefix, token included

// Persisted, so links already sent keep working across a restart.
static void artifacts_token(char *out, size_t size)
{
    char *saved = state_read("artifacts_token");
    if (saved && strlen(saved) >= 16) {
        snprintf(out, size, "%s", saved);
        free(saved);
        return;
    }
    free(saved);

    unsigned char raw[8];
    arc4random_buf(raw, sizeof raw);
    char hex[2 * sizeof raw + 1];
    for (size_t i = 0; i < sizeof raw; i++)
        snprintf(hex + i * 2, 3, "%02x", raw[i]);
    snprintf(out, size, "%s", hex);
    state_write("artifacts_token", hex);
}

// Serve a directory of build products over HTTP so a turn can answer with a
// link. Failure is not fatal: the bridge works, it just cannot link.
static void artifacts_init(void)
{
    const char *dir = cfg_get("artifacts_dir", NULL);
    if (dir && *dir) {
        snprintf(art_dir, sizeof art_dir, "%s", dir);
    } else {
        char base[4096];
        if (!path_config_dir(base, sizeof base))
            return;
        snprintf(art_dir, sizeof art_dir, "%s/artifacts", base);
    }
    mkdir(art_dir, 0700);

    int port = (int)cfg_get_long("artifacts_port", 8787);

    // The link has to open on the phone, and nothing beyond the user's own
    // devices should be able to fetch it — so the tailnet address, when there
    // is one, is the right default.
    const char *bind = cfg_get("artifacts_bind", NULL);
    if (!bind || !*bind)
        bind = httpd_tailscale_ip();
    if (!bind || !*bind)
        bind = "127.0.0.1";

    char token[64];
    artifacts_token(token, sizeof token);

    server = httpd_start(art_dir, bind, port, token);
    if (!server) {
        note_up("no artifact server on %s:%d (port taken?)", bind, port);
        return;
    }

    const char *url = cfg_get("artifacts_url", NULL);
    if (url && *url) {
        char trimmed[256];
        snprintf(trimmed, sizeof trimmed, "%s", url);
        size_t l = strlen(trimmed);
        while (l && trimmed[l - 1] == '/')
            trimmed[--l] = '\0';
        snprintf(art_base, sizeof art_base, "%s/%s", trimmed, token);
    } else {
        snprintf(art_base, sizeof art_base, "http://%s:%d/%s", bind, port, token);
    }
}

// Newest first — what was just built is what is being asked about.
struct artifact { char name[256]; time_t mtime; };

static int artifact_newer(const void *a, const void *b)
{
    const struct artifact *x = a, *y = b;
    return x->mtime < y->mtime ? 1 : x->mtime > y->mtime ? -1 : 0;
}

static void send_artifacts(void)
{
    if (!server) {
        send_note("no artifact server running — see the log for why");
        return;
    }
    char msg[1200];
    size_t n = 0;
    appendf(msg, sizeof msg, &n, "%s/\n", art_base);

    struct artifact ents[64];
    int count = 0;
    DIR *d = opendir(art_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && count < (int)(sizeof ents / sizeof *ents)) {
            if (de->d_name[0] == '.')
                continue;
            char full[4400];
            snprintf(full, sizeof full, "%s/%s", art_dir, de->d_name);
            struct stat st;
            if (stat(full, &st) != 0)
                continue;
            snprintf(ents[count].name, sizeof ents[count].name, "%s", de->d_name);
            ents[count].mtime = st.st_mtime;
            count++;
        }
        closedir(d);
    }
    qsort(ents, (size_t)count, sizeof *ents, artifact_newer);

    if (!count)
        appendf(msg, sizeof msg, &n, "\nnothing published yet");
    for (int i = 0; i < count && i < 10; i++)
        appendf(msg, sizeof msg, &n, "\n%s/%s", art_base, ents[i].name);
    if (count > 10)
        appendf(msg, sizeof msg, &n, "\n… and %d more", count - 10);
    send_pre(msg);
}

// ---- what the agent is told ---------------------------------------------

const char *tg_system_note(void)
{
    static char note[12288];
    size_t n = 0;

    appendf(note, sizeof note, &n,
        headless_mode
        ? "## You are being reached over Telegram\n\n"
          "This conversation reaches the user as chat messages on their phone. "
        : "## This conversation is also on Telegram\n\n"
          "The user is at a terminal, but the same session is reachable from "
          "their phone and what you say may be relayed there as chat messages. "
        "Markdown renders; wide tables and long code listings do not. Keep "
        "answers short and say the answer first. To show them a picture — a "
        "render, a screenshot, a photo — write it as a markdown image with an "
        "absolute local path, ![alt](/abs/path.png), and it is sent as a photo; "
        "the file has to be on this machine.\n");

    if (server && art_base[0])
        appendf(note, sizeof note, &n,
            "\n## Artifacts\n\n"
            "Anything you put in %s is served at %s/<name>, a link the user can "
            "open on their phone. When a turn produces something better seen "
            "than pasted — a PDF, an image, a rendered page, a long report — put "
            "it there and reply with its link. Copying or symlinking the file in "
            "both work. Say what the link is before you send it; a bare URL is "
            "not an answer.\n", art_dir, art_base);

    appendf(note, sizeof note, &n,
        "\n## Reminders\n\n"
        "You can set reminders for the user. Reminders are JSON lines in the file "
        "%s. Each has `text` plus a schedule -- either a one-shot/interval or a "
        "recurrence rule:\n"
        "  one-shot / interval: {\"at\":\"YYYY-MM-DD HH:MM\",\"text\":\"...\","
        "\"repeat_secs\":0}  (`at` is 24h LOCAL time; repeat_secs>0 re-fires every N "
        "seconds, 0 = one-shot).\n"
        "  recurrence (OMIT `at` -- the daemon computes the next occurrence): "
        "{\"text\":\"...\",\"rule\":{...}} where rule is one of: "
        "{\"kind\":\"daily\",\"time\":\"13:00\"}; "
        "{\"kind\":\"weekdays\",\"time\":\"13:00\"} (Mon-Fri); "
        "{\"kind\":\"weekly\",\"days\":[\"tue\",\"thu\"],\"time\":\"09:00\"}; "
        "{\"kind\":\"nth_weekday\",\"n\":2,\"dow\":\"tue\",\"time\":\"09:00\"} (2nd "
        "Tuesday each month; n 1-5, or -1 for last); "
        "{\"kind\":\"monthly\",\"dom\":15,\"time\":\"09:00\"} (the 15th). `time` is "
        "24h HH:MM. So \"every weekday at 1pm\" -> weekdays/13:00; \"every second "
        "Tuesday\" -> nth_weekday n=2 dow=tue.\n"
        "When the user gives a DAY but no time (\"tomorrow\", \"on Tuesday\"), "
        "default the time to 08:00. To ADD a reminder, append one line to that file "
        "with your tools. To LIST or CANCEL, read/edit that file. The daemon "
        "delivers due reminders and reschedules recurring ones automatically -- do "
        "not deliver them yourself.\n"
        "A reminder's `text` may be an INSTRUCTION to YOU, not just a message: when "
        "it comes due the daemon hands it back to you to act on. Use this for "
        "DYNAMIC reminders that should reflect current data at delivery time rather "
        "than a frozen snapshot -- e.g. store text like \"List the current land "
        "todos: grep the wiki for todos on the land page\" so the list is re-queried "
        "fresh when it fires. IMPORTANT: a fired reminder's reply is delivered "
        "straight to the user over the chat, so DO NOT phrase reminders as "
        "texting/emailing/\"sending\" them anywhere, and never message or email "
        "another PERSON when a reminder fires unless the user's original request "
        "explicitly said to send it to a named person. The default is simply: "
        "surface the info to the user.\n"
        "PROACTIVITY: when he mentions an upcoming trip or event that maps to stored "
        "todos or info (\"I'm going to the land tomorrow\", \"Costco run Saturday\"), "
        "OFFER to schedule a morning reminder that surfaces the relevant items -- ask "
        "first, do NOT auto-create it and do NOT just acknowledge the statement.\n",
        reminders_path());

    // Only when the chat is the only way in: at a terminal the user is watching
    // the turn, and telling the session to hand everything to a subagent would
    // change how it works there.
    if (headless_mode)
        appendf(note, sizeof note, &n,
        "\n## Stay responsive: delegate\n\n"
        "You are reached over a chat, and you are the user's ONLY point of "
        "contact there: while a turn is running they cannot ask you anything "
        "else, and every message they send waits in a queue behind it. So keep "
        "turns SHORT and hand the long work to subagents.\n"
        "The rule: if answering needs more than about one or two tool calls -- "
        "any code change, investigation, build, test run, search across files, "
        "or anything you expect to take more than ~30 seconds -- spawn a "
        "BACKGROUND subagent and end your turn with a one-line acknowledgement "
        "of what you started. Do NOT wait for it, and do NOT narrate the plan "
        "at length. Trivia you can answer from context, a single file read, a "
        "one-line question -- answer those directly; do not delegate a "
        "one-liner.\n"
        "Launch independent subagents in parallel rather than chaining them. "
        "Give each a specific, self-contained brief -- it does not see this "
        "conversation -- and a `description` the user will recognise in a status "
        "list, because that is what /agents shows them.\n"
        "Keep the fan-out FLAT and SMALL: at most 3 subagents per turn, and one "
        "level deep. On Claude Code use `subagent_type: \"worker\"` -- it has no "
        "Agent tool and runs on a cheaper model. Do NOT use general-purpose for "
        "delegated work; it can spawn its own agents, and a tree of them will "
        "burn the session limit in minutes.\n"
        "Nothing you are reading now reaches a subagent, so on any backend "
        "without that agent type, say it in the brief instead -- end every one "
        "with: \"Do this work yourself. Do not spawn subagents, and do not "
        "sleep-poll -- if something is blocked or rate-limited, report it as a "
        "gap and finish.\"\n"
        "Collecting the results: whenever you get a turn and background work is "
        "outstanding, CHECK on it before you answer, and report anything that "
        "has landed. Relay what matters in plain prose -- the user has not seen "
        "the subagent's work, only its description.\n");

    return note;
}

// ---- reminders ----------------------------------------------------------

// Queue any reminders that have come due. The text is an instruction to the
// agent rather than a message from the user, so it carries its own framing; the
// reply is what actually reaches the chat.
static void fire_due_reminders(void)
{
    for (int guard = 0; guard < 64; guard++) {
        char rem[2048];
        if (!reminders_pop_due(time(NULL), rem, sizeof rem))
            break;

        static const char fmt[] =
            "A scheduled reminder just came due: \"%s\". Deliver it to the user "
            "now -- your reply goes straight to THEM over the chat, so just tell "
            "them (if it's an instruction to gather info, do that and reply with "
            "the result). Do NOT text or email it to anyone, and do NOT "
            "message/email another person unless this reminder explicitly names "
            "sending it to a specific person.";
        int need = snprintf(NULL, 0, fmt, rem);
        if (need < 0)
            break;
        char *p = malloc((size_t)need + 1);
        if (!p)
            break;
        snprintf(p, (size_t)need + 1, fmt, rem);
        if (!inbox_push(p, 0))
            break;
    }
}

// ---- the poller ---------------------------------------------------------

static int poller_aborting(void) { return poller_stop; }

// The daemon case: nothing else is listening for the signal, and the agent CLI
// is a child that would otherwise be orphaned holding the session.
static void on_signal(int sig)
{
    (void)sig;
    quit_wanted = 1;
    poller_stop = 1;
    wake_up();
}

static const char *ext_for(const tg_update *u)
{
    const char *dot = u->file_name ? strrchr(u->file_name, '.') : NULL;
    if (dot && strlen(dot) < 8)
        return dot + 1;
    if (u->mime_type) {
        if (!strcmp(u->mime_type, "image/jpeg"))
            return "jpg";
        if (!strcmp(u->mime_type, "image/png"))
            return "png";
        if (!strcmp(u->mime_type, "image/webp"))
            return "webp";
        if (!strcmp(u->mime_type, "application/pdf"))
            return "pdf";
        if (!strncmp(u->mime_type, "audio/", 6))
            return "ogg";
    }
    return "bin";
}

// A message being assembled from one update, or from a whole album.
struct incoming {
    char *text;
    char *files[MAX_ATTACH];
    int   nfiles;
};

// Download one attachment into /tmp and record its path. Voice and audio are
// transcribed locally (whisper.h) and the transcript becomes the text.
static void take_file(const tg_update *u, struct incoming *in)
{
    if (in->nfiles >= MAX_ATTACH)
        return;
    char path[600];
    snprintf(path, sizeof path, "/tmp/" APP_NAME "_tg_%ld.%s", u->update_id, ext_for(u));
    if (tg_download_file(rx, u->file_id, path) != 0)
        return;

    if (u->mime_type && !strncmp(u->mime_type, "audio/", 6)) {
        char *said = whisper_transcribe(path, voice_set ? &voice : NULL);
        remove(path);
        if (said) {
            free(in->text);
            in->text = said;
        }
        return;
    }
    in->files[in->nfiles++] = strdup(path);
}

// Attachments are named in the line so the agent reads them with its own tools.
static char *compose(struct incoming *in)
{
    if (!in->nfiles)
        return in->text ? in->text : strdup("");
    size_t n = 256;
    for (int i = 0; i < in->nfiles; i++)
        n += strlen(in->files[i]) + 8;
    if (in->text)
        n += strlen(in->text);
    char *out = malloc(n);
    if (!out)
        return in->text ? in->text : strdup("");
    size_t at = (size_t)snprintf(out, n, "I sent %s:\n",
                                 in->nfiles == 1 ? "a file" : "some files");
    for (int i = 0; i < in->nfiles; i++) {
        at += (size_t)snprintf(out + at, n - at, "- %s\n", in->files[i]);
        free(in->files[i]);
    }
    if (in->text && *in->text)
        snprintf(out + at, n - at, "\n%s", in->text);
    free(in->text);
    return out;
}

// A message that says nothing but "stop" — the word alone is what gets typed
// (or dictated, hence the trailing punctuation) when a turn should end.
static int is_bare_stop(const char *s)
{
    if (!s)
        return 0;
    while (*s == ' ' || *s == '\t' || *s == '\n')
        s++;
    if (strncasecmp(s, "stop", 4) != 0)
        return 0;
    s += 4;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '.' ||
           *s == '!' || *s == '?')
        s++;
    return *s == '\0';
}

static void *poller_thread(void *ud)
{
    (void)ud;
    char *saved = state_read("offset");
    long offset = saved ? strtol(saved, NULL, 10) : 0;
    free(saved);

    while (!poller_stop) {
        fire_due_reminders();       // before the long poll: runs even if it errors

        tg_update *u = NULL;
        int n = tg_get_updates(rx, offset, poll_seconds, &u);
        if (n < 0) {
            // Aborting reads as a failed poll, so the wait before retrying has
            // to notice the stop rather than sitting out its two seconds.
            for (int i = 0; i < 20 && !poller_stop; i++) {
                struct timespec nap = {0, 100 * 1000 * 1000};
                nanosleep(&nap, NULL);
            }
            continue;
        }

        for (int i = 0; i < n && !poller_stop; i++) {
            offset = u[i].update_id + 1;

            if (u[i].chat_id != chat_id)
                continue;

            // A tapped button. The spinner on the sender's phone runs until
            // the tap is acknowledged, so that happens here rather than
            // wherever the queue gets to it.
            if (u[i].callback_data) {
                tg_answer_callback(rx, u[i].callback_id, NULL);
                char *tapped = strdup(u[i].callback_data);
                if (tapped && !inbox_push_kind(tapped, 0, 1))
                    tg_send_message(rx, chat_id, "queue is full; try again shortly");
                continue;
            }

            // Stop must not queue behind the turn it is meant to cancel.
            if (u[i].text && (!strcmp(u[i].text, "/stop") || is_bare_stop(u[i].text))) {
                stop_wanted = 1;
                wake_up();
                continue;
            }

            struct incoming in = {0};
            if (u[i].text)
                in.text = strdup(u[i].text);
            if (u[i].file_id)
                take_file(&u[i], &in);

            // An album arrives as several updates sharing a media_group_id;
            // fold the rest of this batch into the same message.
            while (u[i].media_group_id && i + 1 < n && u[i + 1].media_group_id &&
                   !strcmp(u[i].media_group_id, u[i + 1].media_group_id)) {
                i++;
                offset = u[i].update_id + 1;
                if (u[i].file_id)
                    take_file(&u[i], &in);
                if (!in.text && u[i].text)
                    in.text = strdup(u[i].text);
            }

            char *line = compose(&in);
            if (line && *line && !inbox_push(line, 0))
                tg_send_message(rx, chat_id, "queue is full; try again shortly");
            else if (line && !*line)
                free(line);
        }
        tg_updates_free(u, n);

        char buf[32];
        snprintf(buf, sizeof buf, "%ld", offset);
        state_write("offset", buf);
    }
    return NULL;
}

// ---- the chat's tabs ----------------------------------------------------

// The conversations the chat can reach. Headless they are its own, held by
// chattabs; with a terminal they are the window's, held by the workspace.
//
// Either way there is one current tab, not one per front end: switching from
// the phone switches the terminal, and switching at the terminal is what the
// phone is then talking to. The chat is a second screen onto the window, so
// what it does has to be what the window did.

static int tab_count(void)
{
    return headless_mode ? chattabs_count() : workspace_count();
}

static struct session *tab_at(int i)
{
    return headless_mode ? chattabs_at(i) : workspace_at(i);
}

static int tab_index(void)
{
    return headless_mode ? chattabs_index() : workspace_index();
}

static int tab_find_id(const char *id)
{
    return headless_mode ? chattabs_find_id(id) : workspace_find_id(id);
}

// The observer follows the chat: what another tab does is its own business
// until it finishes.
static void focus(struct session *s)
{
    if (sess == s)
        return;
    // The tab left behind goes back to being nobody's business here: it keeps
    // working, and keeps its own screen, but what it does stops being
    // relayed.
    if (sess)
        session_set_observer(sess, NULL, NULL);
    sess = s;
    if (!sess)
        return;
    session_set_observer(sess, on_event, NULL);
    session_set_abort_hook(sess, on_abort, sess);
}

static int tab_switch(int i)
{
    if (i < 0 || i >= tab_count())
        return 0;
    if (headless_mode)
        chattabs_show(i);
    else
        workspace_show(i);      // the terminal comes along; it is the same tab
    focus(tab_at(i));
    return 1;
}

// Starts one and points the chat at it, without moving what the terminal
// shows.
static int tab_open(const char *cwd, const char *id)
{
    if (headless_mode)
        return chattabs_open(cwd, id);

    struct session *from = workspace_current();
    if (!from)
        return -1;
    // The workspace brings a new tab up as it opens, which is what the chat
    // wants too: it is about to be talking to it.
    return workspace_spawn(session_backend(from), session_model(from),
                           session_effort(from),
                           cwd && *cwd ? cwd : session_cwd(from), id);
}

static int tab_drop(int i)
{
    if (headless_mode)
        return chattabs_close(i);
    workspace_close(i);
    return workspace_count();
}

// The last part of a path, which is what a directory is called in practice.
static const char *dir_name(const char *path)
{
    if (!path || !*path)
        return "?";
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

// How a tab reads in a list: where it is, and what it is doing.
static void tab_label(int i, char *out, size_t size)
{
    struct session *s = tab_at(i);
    const char *title = session_title(s);
    if (!title || !*title)
        title = dir_name(session_cwd(s));

    const char *mark = i == tab_index()  ? "> "
                     : session_unseen(s) ? "* "
                                         : "  ";
    const char *what = session_turn_running(s) ? "  (working)" : "";
    snprintf(out, size, "%s%d  %s%s", mark, i + 1, title, what);
}

// What a menu row means for the "tab" kind: the conversation itself when the
// backend has named it, and where it sits when it has not yet.
static void tab_payload(int i, char *out, size_t size)
{
    const char *id = session_id(tab_at(i));
    if (id && *id)
        snprintf(out, size, "%s", id);
    else
        snprintf(out, size, "#%d", i);
}

static int tab_from_payload(const char *payload)
{
    if (!payload || !*payload)
        return -1;
    if (*payload == '#') {
        int at = atoi(payload + 1);
        return at >= 0 && at < tab_count() ? at : -1;
    }
    return tab_find_id(payload);
}

// Says which conversation the chat is now talking to. Worth saying on every
// switch: with no tab strip in front of you it is the only thing that does.
static void send_here(void)
{
    struct session *s = sess;
    if (!s)
        return;
    const char *title = session_title(s);
    send_notef("%d/%d  %s  in %s", tab_index() + 1, tab_count(),
               title && *title ? title : "untitled", dir_name(session_cwd(s)));
}

static void send_tabs(void)
{
    int n = tab_count();
    if (n <= 0) {
        send_note("no conversations here");
        return;
    }
    menu_begin("tab");
    for (int i = 0; i < n; i++) {
        char label[80], payload[200];
        tab_label(i, label, sizeof label);
        tab_payload(i, payload, sizeof payload);
        menu_add(label, payload);
    }
    if (n < (headless_mode ? CHATTABS_MAX : WORKSPACE_MAX))
        menu_add("+ new conversation", "new");
    menu_add("resume one from here...", "resume");
    menu_send("conversations", 1);
}

// The past conversations of this directory, newest first, as a menu.
static void send_resume(void)
{
    if (!sess) {
        send_note("no conversation to resume alongside");
        return;
    }
    const char *backend = session_backend(sess);
    if (!sessionlist_available(backend)) {
        send_notef("%s keeps no list of past conversations", backend);
        return;
    }

    struct past_session *past = NULL;
    int n = sessionlist_load(backend, session_cwd(sess), session_id(sess), &past);
    if (n <= 0) {
        free(past);
        send_note("nothing to resume here");
        return;
    }

    menu_begin("resume");
    for (int i = 0; i < n && i < MENU_MAX; i++) {
        char label[80];
        snprintf(label, sizeof label, "%s  %s", past[i].when, past[i].label);
        menu_add(label, past[i].id);
    }
    free(past);
    char title[80];
    snprintf(title, sizeof title, "past conversations here%s",
             n > MENU_MAX ? " (the newest few)" : "");
    menu_send(title, 1);
}

// Opens one, in `cwd` when it is named, and points the chat at it.
static void open_tab(const char *cwd, const char *id)
{
    int max = headless_mode ? CHATTABS_MAX : WORKSPACE_MAX;
    if (tab_count() >= max) {
        send_notef("that is all %d conversations; close one first", max);
        return;
    }
    char *expanded = cwd && *cwd ? path_expand_home(cwd) : NULL;
    char  resolved[4096];
    const char *where = NULL;
    if (cwd && *cwd) {
        if (!realpath(expanded ? expanded : cwd, resolved)) {
            send_notef("no such directory: %s", cwd);
            free(expanded);
            return;
        }
        where = resolved;
    }
    free(expanded);

    int at = tab_open(where, id);
    if (at < 0) {
        send_note("could not start another conversation");
        return;
    }
    focus(tab_at(at));
    send_here();
}

static void close_tab(int at)
{
    if (at < 0 || at >= tab_count()) {
        send_note("no such conversation");
        return;
    }
    if (tab_count() == 1) {
        send_note("that is the only conversation; /quit to stop instead");
        return;
    }
    int mine = at == tab_index();
    tab_drop(at);
    // Dropping the chat's own tab leaves it standing nowhere.
    if (mine || !sess)
        focus(headless_mode ? chattabs_current() : workspace_current());
    send_here();
}

static void switch_tab(int at)
{
    if (!tab_switch(at)) {
        send_note("no such conversation");
        return;
    }
    send_here();
}

void tg_refocus(void)
{
    if (!running)
        return;
    focus(headless_mode ? chattabs_current() : workspace_current());
}

// A conversation opened from the chat, headless, is set up the way the first
// one was. The observer is not among these: it goes on whichever tab the chat
// is pointed at, and open_tab() points it at this one straight after.
static void prepare_tab(struct session *s)
{
    session_set_silent(s, 1);
    session_set_system_extra(s, tg_system_note());
    session_set_abort_hook(s, on_abort, s);
}

// A tapped row as the line it stands for. Everything else reaches the bridge
// as a line, so a tap becomes one nobody had to type, and runs where a typed
// one runs rather than wherever it happened to arrive.
static char *menu_line(const char *tapped)
{
    char kind[16], payload[200], line[256];
    if (!menu_take(tapped, kind, sizeof kind, payload, sizeof payload))
        return NULL;

    if (!strcmp(kind, "tab")) {
        if (!strcmp(payload, "new")) {
            snprintf(line, sizeof line, "/open");
        } else if (!strcmp(payload, "resume")) {
            snprintf(line, sizeof line, "/resume");
        } else {
            int at = tab_from_payload(payload);
            if (at < 0) {
                send_note("that conversation is gone");
                return NULL;
            }
            snprintf(line, sizeof line, "/tab %d", at + 1);
        }
    } else if (!strcmp(kind, "resume")) {
        snprintf(line, sizeof line, "/resume %s", payload);
    } else {
        return NULL;
    }
    return strdup(line);
}

// ---- running a line -----------------------------------------------------

static void send_bridge_status(void)
{
    char msg[1400];
    size_t n = 0;
    appendf(msg, sizeof msg, &n, "%-10s %s\n", "bridge", label);
    appendf(msg, sizeof msg, &n, "%-10s %ld\n", "chat", chat_id);
    appendf(msg, sizeof msg, &n, "%-10s %s\n", "mirror",
            mirror == MIRROR_OFF ? "off" : mirror == MIRROR_REMOTE ? "remote" : "all");
    appendf(msg, sizeof msg, &n, "%-10s %s\n", "front end",
            headless_mode ? "chat only" : "shared with a terminal");
    if (tab_count() > 1)
        appendf(msg, sizeof msg, &n, "%-10s %d of %d\n", "tab",
                tab_index() + 1, tab_count());
    appendf(msg, sizeof msg, &n, "%-10s %ds\n", "poll", poll_seconds);
    appendf(msg, sizeof msg, &n, "%-10s %s\n", "artifacts",
            server ? art_base : "not running");
    appendf(msg, sizeof msg, &n, "%-10s %d scheduled\n", "reminders",
            reminders_scheduled_count());
    appendf(msg, sizeof msg, &n, "%-10s %d running, %d this session\n", "agents",
            subagents_running(), agent_count);
    pthread_mutex_lock(&log_lock);
    if (last_log[0]) {
        char extra[32] = "";
        if (log_repeats)
            snprintf(extra, sizeof extra, " (x%d)", log_repeats + 1);
        appendf(msg, sizeof msg, &n, "%-10s %s%s\n", "last error", last_log, extra);
    }
    pthread_mutex_unlock(&log_lock);
    send_pre(msg);
}

static const char *HELP =
    "Anything you type is one turn in a live mux session, including the CLI's "
    "own slash commands (/w, /email, /code-review, ...) and mux's (/model, "
    "/new, /cd, /backend, /session, ...).\n\n"
    "With a terminal attached these are that window's own tabs: switching "
    "here switches what the terminal shows, and the other way round.\n\n"
    "/tabs        the conversations open here, to switch between\n"
    "             (/sessions is the same list)\n"
    "/open [dir]  another conversation, here or somewhere else\n"
    "/close [n]   drop one\n"
    "/resume      reopen a past conversation from this directory\n"
    "/agents      background agents: status, runtime, latest\n"
    "/artifacts   published files and their links\n"
    "/stop        abandon the turn in flight (or just say \"stop\")\n"
    "/tg          the bridge's own settings, and this\n\n"
    "Settings live in ~/.config/mux/telegram.";

// Commands that need a terminal, and are refused rather than left to hang.
static int needs_terminal(const char *line)
{
    static const char *const local[] = { "/fh", "/fs", "/fv", "/fw", "/mux",
                                         "/settings", "/copy", NULL };
    for (int i = 0; local[i]; i++) {
        size_t n = strlen(local[i]);
        if (!strncmp(line, local[i], n) && (!line[n] || line[n] == ' '))
            return 1;
    }
    return 0;
}

// The bridge's own commands, which the terminal has no use for.
// The word after the command, or NULL. Also answers whether `line` is that
// command at all.
static const char *arg_of(const char *line, const char *cmd)
{
    size_t n = strlen(cmd);
    if (strncmp(line, cmd, n) || (line[n] && line[n] != ' '))
        return NULL;
    const char *arg = line + n;
    while (*arg == ' ')
        arg++;
    return arg;                 // "" when the command stood alone
}

static int bridge_command(const char *line)
{
    // /sessions is the terminal's name for the same list, and reaches a
    // picker with nowhere to draw when the ask came from the chat.
    if (!strcmp(line, "/tabs") || !strcmp(line, "/tab") ||
        !strcmp(line, "/sessions")) {
        send_tabs();
        return 1;
    }
    const char *arg = arg_of(line, "/tab");
    if (arg && *arg) {
        switch_tab(atoi(arg) - 1);
        return 1;
    }
    if ((arg = arg_of(line, "/open")) != NULL) {
        open_tab(*arg ? arg : NULL, NULL);
        return 1;
    }
    if ((arg = arg_of(line, "/close")) != NULL) {
        close_tab(*arg ? atoi(arg) - 1 : tab_index());
        return 1;
    }
    if ((arg = arg_of(line, "/resume")) != NULL) {
        // With an id behind it the row of a menu was tapped; on its own it is
        // the ask for that menu.
        if (*arg) {
            int at = tab_find_id(arg);
            if (at >= 0)
                switch_tab(at);         // already open here
            else
                open_tab(NULL, arg);
        } else {
            send_resume();
        }
        return 1;
    }
    if (!strcmp(line, "/agents")) {
        send_agents();
        return 1;
    }
    if (!strcmp(line, "/artifacts")) {
        send_artifacts();
        return 1;
    }
    if (!strcmp(line, "/tg")) {
        send_bridge_status();
        send_markdown(HELP);
        return 1;
    }
    if (!strcmp(line, "/stop"))
        return 1;               // handled by the poller; nothing left to do
    return 0;
}

// Relay what the turn ended on, unless the stream already said it.
static void send_turn_reply(int ok, int quiet)
{
    if (!ok) {
        const char *why = session_last_error(sess);
        char msg[700];
        snprintf(msg, sizeof msg, "the turn failed%s%s", why ? ":\n" : "",
                 why ? why : "");
        send_markdown(msg);
        return;
    }
    const char *reply = session_last_reply(sess);
    int nothing_said = (!reply || !*reply) && (!last_said || !*last_said);
    if (quiet && nothing_said)
        return;                 // nobody asked; an empty answer is not news
    if (session_last_interrupted(sess) && (!reply || !*reply)) {
        send_note("(stopped)");
        return;
    }
    if (reply && *reply && (!last_said || strcmp(last_said, reply))) {
        send_markdown(reply);
        send_images(reply);
    }
    int pct = session_context_percent(sess);
    long window = session_context_window(sess);
    if (pct > 0 && window > 0)
        send_notef("context %d%% of %ldk", pct, window / 1000);
}

// The turn the chat asked for. With other conversations open it cannot simply
// be waited on: they have turns of their own to advance, and a tab left
// working while the chat looks elsewhere is the reason they exist. So the turn
// runs on its own thread and this pumps every tab until it ends.
static int run_turn(const char *line)
{
    if (!headless_mode || chattabs_count() < 2)
        return session_turn(sess, line);

    struct session *s = sess;
    if (!session_turn_begin(s, line))
        return 0;

    while (session_turn_running(s)) {
        int fds[CHATTABS_MAX + 1];
        int n = chattabs_fds(fds, CHATTABS_MAX);
        int max = -1;
        fd_set set;
        FD_ZERO(&set);
        for (int i = 0; i < n; i++) {
            FD_SET(fds[i], &set);
            if (fds[i] > max)
                max = fds[i];
        }
        struct timeval tv = {0, 100 * 1000};
        select(max + 1, max >= 0 ? &set : NULL, NULL, NULL, &tv);
        chattabs_pump();
    }
    return session_failed_prompt(s) == NULL;
}

// One line from the chat, run exactly as the prompt would run it: a bash
// escape, a mux command, or a turn.
static void run_line(char *line, int quiet)
{
    menu_flush();
    free(last_said);
    last_said = NULL;
    from_chat = 1;
    frontend_push(0);   // the chat has no keyboard behind it
    nudge_queued = 0;
    repeat_task = 0;
    stop_wanted = 0;    // a stop sent before this line was meant for the last

    if (bridge_command(line))
        goto done;

    // The tab the chat was standing in has been closed at the terminal. There
    // is usually another to stand in.
    if (!sess && !headless_mode)
        focus(workspace_current());
    if (!sess) {
        send_note("that session is gone — start a new one at the terminal");
        goto done;
    }

    if (headless_mode && needs_terminal(line)) {
        send_notef("%s needs the terminal; not available over the chat", line);
        goto done;
    }

    // A bash escape needs a keyboard: the command writes to the terminal and may
    // want to be answered. Over the chat alone the line is just text, and the
    // agent runs what it needs with its own tools.
    if (!headless_mode && bash_is_command(line)) {
        bash_run(line);
        gitinfo_forget();
        char *context = bash_take_context();
        if (context) {
            send_pre(context);
            send_turn_reply(run_turn(context), 0);
            cmd_run_deferred(sess);
            free(context);
        }
        goto done;
    }

    if (headless_mode)
        ui_sink_begin();
    else
        ui_sink_begin_tee();
    enum cmd_result r = cmd_dispatch(sess, line);
    char *shown = ui_sink_end();
    if (r != CMD_NOT_A_COMMAND) {
        strip_ansi(shown);
        if (shown && *shown)
            send_pre(shown);
        else
            send_note("ok");
    }
    free(shown);

    if (r == CMD_QUIT) {
        if (headless_mode) {
            send_note("stopping");
            quit_wanted = 1;
        } else {
            send_note("the terminal owns this session; /quit there");
        }
        goto done;
    }
    if (r == CMD_NOT_A_COMMAND) {
        if (!headless_mode)
            status_sticky_prompt(line);
        send_turn_reply(run_turn(line), quiet);
        cmd_run_deferred(sess);
    }

done:
    from_chat = 0;
    frontend_pop();
    free(line);
}

// ---- the front end ------------------------------------------------------

// The bot to be. A token in the environment is the default and the one to
// prefer — `token_env` picks which variable, so a second bot is a config change
// rather than an edit here. A token written in the config file works too, for
// a daemon started by something with no environment to speak of.
static const char *bot_token(void)
{
    const char *var = cfg_get("token_env", "TELEGRAM_TOKEN");
    const char *token = getenv(var);
    if (token && *token)
        return token;
    return cfg_get("token", NULL);
}

int tg_start(struct session *s, int headless)
{
    const char *token = bot_token();
    if (!token || !*token) {
        char path[4200];
        path_config_file(path, sizeof path, "telegram");
        fprintf(stderr, APP_NAME ": --telegram needs a bot token — $%s, or `token` in %s\n",
                cfg_get("token_env", "TELEGRAM_TOKEN"), path);
        return 0;
    }
    chat_id = cfg_get_long("chat_id", 0);
    if (!chat_id) {
        char path[4200];
        path_config_file(path, sizeof path, "telegram");
        fprintf(stderr, APP_NAME ": set chat_id in %s — refusing to serve every chat\n",
                path);
        return 0;
    }

    // With no terminal there is nothing to mirror from: everything the agent
    // does is only visible in the chat.
    // At the terminal the phone is a second screen, not the only one, and every
    // mirrored event is an HTTP round trip in the middle of the turn. So the
    // default there is to mirror only what the chat itself asked for.
    const char *m = headless ? "all" : cfg_get("mirror", "remote");
    mirror = !strcmp(m, "off") ? MIRROR_OFF : !strcmp(m, "remote") ? MIRROR_REMOTE
                                                                  : MIRROR_ALL;

    voice.model_path = cfg_get("whisper_model", NULL);
    voice.whisper_bin = cfg_get("whisper_bin", NULL);
    voice.ffmpeg_bin = cfg_get("ffmpeg_bin", NULL);
    voice_set = voice.model_path || voice.whisper_bin || voice.ffmpeg_bin;
    poll_seconds = (int)cfg_get_long("poll_seconds", 30);
    if (poll_seconds < 1 || poll_seconds > 60)
        poll_seconds = 30;

    // What the hud says. The bot's own name is worth showing when there is more
    // than one: which bot answered is not otherwise visible from the terminal.
    const char *bot = cfg_get("bot", NULL);
    snprintf(label, sizeof label, "telegram%s%s", bot ? " " : "", bot ? bot : "");

    rx = tg_new(token);
    tx = tg_new(token);
    if (!rx || !tx) {
        fprintf(stderr, APP_NAME ": telegram init failed\n");
        return 0;
    }
    if (pipe(wake) != 0) {
        fprintf(stderr, APP_NAME ": telegram wake pipe: %s\n", strerror(errno));
        return 0;
    }
    fcntl(wake[0], F_SETFL, O_NONBLOCK);
    fcntl(wake[1], F_SETFL, O_NONBLOCK);

    sess = s;
    headless_mode = headless;
    artifacts_init();
    session_set_system_extra(s, tg_system_note());
    session_set_observer(s, on_event, NULL);
    session_set_abort_hook(s, on_abort, s);
    if (headless) {
        session_set_silent(s, 1);
        chattabs_on_open(prepare_tab);
    }

    tg_set_abort_check(poller_aborting);
    tg_set_log(on_log);
    if (headless) {
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
        restart_flag("--telegram");
    }
    signal(SIGPIPE, SIG_IGN);
    running = 1;
    if (pthread_create(&poller, NULL, poller_thread, NULL) != 0) {
        fprintf(stderr, APP_NAME ": can't start the telegram poller\n");
        running = 0;
        return 0;
    }

    char *note = state_read("restarted");
    if (note && *note) {
        send_note("restarted");
        state_write("restarted", "");
    } else if (cfg_get_long("announce", 0)) {
        char scratch[600];
        snprintf(scratch, sizeof scratch, "%s is live in %s", label, session_cwd(s));
        send_note(scratch);
    }
    free(note);

    note_up("%s, chat %ld%s%s", label, chat_id,
            server ? ", artifacts on " : "", server ? art_base : "");
    return 1;
}

const char *tg_label(void)
{
    return running ? label : NULL;
}

// Only ever called on the way out of the process, and nothing here is worth
// making anyone wait for: the poller is usually parked in a long poll that
// libcurl will not abandon for up to a second, and tearing a curl handle down
// can itself talk to the network. So the port is freed, the threads are cut
// loose to die with the process, and the clients are left unfreed on purpose.
void tg_stop(void)
{
    if (!running)
        return;
    running = 0;
    poller_stop = 1;
    wake_up();
    pthread_detach(poller);
    httpd_close(server);
    server = NULL;
    rx = tx = NULL;
    free(last_said);
    last_said = NULL;
}

// The terminal is sharing this session: hand the line over for the prompt to
// run, so everything happens on the one thread that owns the session.
char *tg_take_line(void)
{
    if (!running)
        return NULL;
    for (;;) {
        int tap = 0;
        char *line = inbox_take(NULL, &tap);
        if (!line || !tap)
            return line;
        char *cmd = menu_line(line);
        free(line);
        if (cmd)
            return cmd;             // the prompt runs it as any chat line
    }
}

// The prompt handed a chat line back for the main thread to run. Takes the line.
struct session *tg_session(void)
{
    return sess;
}

// The session the bridge cached is going away: a tab closing frees it, a
// handoff gives it to another window. Either way the pointer must not be used.
void tg_forget_session(struct session *s)
{
    if (sess == s)
        sess = NULL;
}

void tg_run_line(char *line)
{
    run_line(line, 0);
}

// A conversation the chat was not watching has finished. Which one it was has
// to come first: without it the answer reads as if it came from the tab in
// front.
static void on_background(struct session *s, int index)
{
    char label[80];
    tab_label(index, label, sizeof label);
    send_notef("%s finished", label + 2);        // past the current/unseen mark

    const char *reply = session_last_reply(s);
    if (reply && *reply)
        send_markdown(reply);
    else if (session_failed_prompt(s))
        send_note("it failed; /tab it to see");
}

// Headless: the chat is the only front end, so this loop is the prompt.
void tg_run(struct session *s)
{
    chattabs_on_background(on_background);

    while (!quit_wanted) {
        // Rebuilt while it was running: hand the conversation to the new
        // binary, which resumes this session and says so in the chat.
        if (restart_wanted()) {
            send_note("restarting...");
            state_write("restarted", "1");
            tg_stop();
            restart_exec(s);        // returns only if the new build won't run
            send_note("could not restart — staying on this build");
        }

        // Another window has asked for this conversation. Answering is the
        // caller's to do: it owns the session this loop is holding.
        if (handoff_wanted())
            return;

        int quiet = 0, tap = 0;
        char *line = inbox_take(&quiet, &tap);
        if (line) {
            if (tap) {
                char *cmd = menu_line(line);
                free(line);
                if (cmd)
                    run_line(cmd, 0);       // takes the line
            } else {
                run_line(line, quiet);      // takes the line
            }
            continue;
        }

        // Nothing waiting: read whatever the agents produced on their own. A
        // background subagent finishing wakes some backends with no send from
        // here; on the others it streams its work and then waits to be asked.
        chattabs_pump();
        if (sess && session_idle_busy(sess))
            nudge_for_subagents();

        int fds[CHATTABS_MAX + 1];
        int n = chattabs_fds(fds, CHATTABS_MAX);
        int max = -1;
        fd_set set;
        FD_ZERO(&set);
        if (wake[0] >= 0) {
            FD_SET(wake[0], &set);
            max = wake[0];
        }
        for (int i = 0; i < n; i++) {
            FD_SET(fds[i], &set);
            if (fds[i] > max)
                max = fds[i];
        }
        struct timeval tv = {0, 250 * 1000};
        select(max + 1, max >= 0 ? &set : NULL, NULL, NULL, &tv);
    }
}
