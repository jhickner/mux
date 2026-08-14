/*
 * grok.h — drive the Grok CLI (`grok agent stdio`) from C, mirroring claude.h.
 *
 * Spawns `grok agent stdio` as a persistent subprocess and speaks its ACP
 * (Agent Client Protocol, JSON-RPC 2.0 over stdio): initialize -> session/new
 * (or session/resume) -> session/prompt, accumulating the streamed
 * agent_message_chunk text into the final reply. Tool calls arrive as session/update tool_call /
 * tool_call_update notifications. One process stays alive and retains context
 * across turns.
 *
 *     #define GROK_IMPLEMENTATION      // in exactly one .c file
 *     #include "grok.h"
 *
 *     grok_opts o = {0};
 *     o.cwd = "/path/to/project";
 *     o.append_system = "You are a terse assistant.";  // prepended to each turn
 *     grok_client *c = grok_start(&o);
 *     char *reply = grok_send(c, "say hi");
 *     puts(reply); free(reply);
 *     grok_stop(c);
 *
 * Auth defaults to the cached token in ~/.grok/auth.json (your Grok
 * subscription) — no XAI_API_KEY needed. Depends on cJSON and a `grok` CLI on
 * PATH. ACP has no system-prompt field, so append_system is prepended to each
 * user turn (Grok caches the repeated prefix cheaply).
 */
#ifndef GROK_H
#define GROK_H

typedef struct grok_client grok_client;

typedef struct {
    const char *cli_path;        /* grok binary; NULL/"" -> "grok" (via PATH)   */
    const char *cwd;             /* child working directory; NULL -> inherit    */
    const char *model;           /* -m value (e.g. "grok-4.5"); NULL -> default */
    const char *reasoning_effort;/* --reasoning-effort low|medium|high; NULL    */
    const char *append_system;   /* prepended to each user turn; NULL -> none   */
    const char *resume_session;  /* ACP session/resume this id; NULL -> new     */
    int no_session;              /* nonzero: ask Grok not to persist the session */
} grok_opts;

/* Spawn a persistent `grok agent stdio` process. Returns NULL only on a local
 * setup failure: the ACP handshake (initialize + session/new) waits for the
 * first grok_send, so that the CLI's startup is not charged to the caller
 * before it has anything to ask. A missing or broken `grok` surfaces there. */
grok_client *grok_start(const grok_opts *opts);

/* Complete the deferred ACP handshake without sending a prompt. This is useful
 * for validating a resume before replacing a live client. Returns nonzero once
 * the requested session is live. New clients normally need not call it because
 * grok_send performs the handshake itself. */
int grok_connect(grok_client *c);

/* Send one user turn (plain text) and return the final assistant text (malloc'd,
 * caller frees), or NULL on failure / process death. Blocks until the turn's
 * prompt response, and completes the handshake first when it is still pending.
 * Context is retained across calls by the CLI itself. An interrupted turn
 * still returns the text received so far (possibly "") rather than NULL. */
char *grok_send(grok_client *c, const char *user_text);

/* Accounting from a turn. Fields a driver cannot fill stay 0. */
typedef struct {
    int interrupted;   /* the abort predicate ended the turn */
} grok_result;

/* As grok_send, but also fills *meta (zeroed first). `meta` may be NULL. */
char *grok_send_ex(grok_client *c, const char *user_text, grok_result *meta);

/* The ACP session id, or NULL until the first turn has established one. */
const char *grok_session_id(grok_client *c);

/* When on, grok_send echoes a readable trace of the stream to stderr. */
void grok_set_verbose(grok_client *c, int on);

/* One interesting event from the stream, handed to the event callback. */
typedef enum {
    GROK_EV_ASSISTANT,   /* text: an assistant text chunk                      */
    GROK_EV_THINKING,    /* text: a reasoning chunk                            */
    GROK_EV_TOOL,        /* name + input_json: a tool the model invoked        */
    GROK_EV_TOOL_RESULT, /* text: the tool's output; failed: status was failed */
} grok_event_kind;

typedef struct {
    grok_event_kind kind;
    const char *text;       /* NULL unless the kind documents it */
    const char *name;       /* tool name, for TOOL               */
    const char *input_json; /* tool input as compact JSON, TOOL  */
    int failed;             /* TOOL_RESULT: status was "failed"  */
} grok_event;

/* Per-event callback. Runs on the grok_send thread; the event and its strings
 * are borrowed for the call. Pass NULL to clear. */
void grok_set_event_cb(grok_client *c,
                       void (*cb)(void *ud, const grok_event *ev),
                       void *ud);

/* Ask the agent to abandon the in-flight turn (ACP session/cancel). The turn
 * still ends with a prompt response, so the stream stays usable for the next
 * send. Returns nonzero on success. */
int grok_interrupt(grok_client *c);

/* Abort predicate; while it returns nonzero an in-flight grok_send cancels
 * the turn (via session/cancel) and returns the text so far. The predicate is
 * still polled during the handshake so it can tick a UI, but cancel waits
 * until a session exists. */
void grok_set_abort_check(grok_client *c, int (*cb)(void));

/* Terminate and reap the process, free the client. */
void grok_stop(grok_client *c);

#endif /* GROK_H */

/* ======================================================================== */
/*   IMPLEMENTATION                                                          */
/* ======================================================================== */
#ifdef GROK_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/wait.h>
#include "cJSON.h"

#define GK_TOOL_CAP 64
#define GK_TOOL_ID  80

/* How long the turn loop waits on the child before running the abort predicate
 * again. Also the worst-case delay before a key the caller reads from that
 * predicate reaches the screen, so it sits below the ~30ms an echo can take
 * without being felt as lag. */
#define GK_TICK_MS 20

struct grok_client {
    pid_t pid;
    int   in_fd;
    int   out_fd;
    int  (*abort)(void);
    int   verbose;
    void (*on_event)(void *ud, const grok_event *ev);
    void *on_event_ud;
    char  session_id[128];
    char *cwd;                /* session/new working directory copy         */
    char *sys;                /* append_system copy, prepended to each turn */
    char *resume;             /* session/resume this id; NULL -> session/new */
    int   no_session;         /* use an ephemeral session/new                */
    int   handshake_failed;   /* the deferred handshake was tried and lost  */
    int   next_id;            /* JSON-RPC request id counter                */
    int   abort_latched;      /* ESC arrived before a session was live      */
    int   cancelling;         /* session/cancel has been sent this turn     */
    grok_result *meta;
    char *buf;                /* line-assembly buffer for out_fd            */
    size_t len, cap;
    char  tool_id[GK_TOOL_CAP][GK_TOOL_ID];
    unsigned tool_flags[GK_TOOL_CAP]; /* bit0 announced, bit1 resulted      */
    char *tool_text[GK_TOOL_CAP];     /* reason/output accumulated per call */
    char  tool_name[GK_TOOL_CAP][64]; /* name resolved across split updates */
    int   n_tools;
};

void grok_set_verbose(grok_client *c, int on) { if (c) c->verbose = on; }
void grok_set_abort_check(grok_client *c, int (*cb)(void)) { if (c) c->abort = cb; }
void grok_set_event_cb(grok_client *c,
                       void (*cb)(void *ud, const grok_event *ev),
                       void *ud) {
    if (c) { c->on_event = cb; c->on_event_ud = ud; }
}
const char *grok_session_id(grok_client *c) {
    return (c && c->session_id[0]) ? c->session_id : NULL;
}

static void gk_emit(grok_client *c, const grok_event *ev) {
    if (c->verbose) {
        const char *kind =
            ev->kind == GROK_EV_ASSISTANT   ? "assistant" :
            ev->kind == GROK_EV_THINKING    ? "thinking"  :
            ev->kind == GROK_EV_TOOL        ? "tool"      :
            ev->kind == GROK_EV_TOOL_RESULT ? "tool-result" : "?";
        const char *text = ev->text ? ev->text : ev->name ? ev->name : "";
        fprintf(stderr, "  [%s] %.400s%s\n", kind, text, strlen(text) > 400 ? " ..." : "");
    }
    if (c->on_event) c->on_event(c->on_event_ud, ev);
}

/* Write one JSON-RPC object as a newline-delimited line. Frees `obj`. */
static int gk_write(grok_client *c, cJSON *obj) {
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json) return 0;
    size_t jl = strlen(json);
    char *withnl = realloc(json, jl + 2);
    if (!withnl) { free(json); return 0; }
    json = withnl; json[jl] = '\n'; json[jl + 1] = '\0';
    int ok = 1;
    for (size_t off = 0; off < jl + 1; ) {
        ssize_t w = write(c->in_fd, json + off, (jl + 1) - off);
        if (w < 0) { if (errno == EINTR) continue; ok = 0; break; }
        off += (size_t)w;
    }
    free(json);
    return ok;
}

int grok_interrupt(grok_client *c) {
    if (!c || !c->session_id[0]) return 0;
    cJSON *n = cJSON_CreateObject();
    cJSON_AddStringToObject(n, "jsonrpc", "2.0");
    cJSON_AddStringToObject(n, "method", "session/cancel");
    cJSON *p = cJSON_AddObjectToObject(n, "params");
    cJSON_AddStringToObject(p, "sessionId", c->session_id);
    c->cancelling = 1;
    return gk_write(c, n);
}

static void gk_reset_tools(grok_client *c) {
    for (int i = 0; i < c->n_tools; i++) {
        free(c->tool_text[i]);
        c->tool_text[i] = NULL;
    }
    c->n_tools = 0;
    memset(c->tool_flags, 0, sizeof c->tool_flags);
}

static int gk_tool_slot(grok_client *c, const char *id) {
    if (!id || !*id) return -1;
    for (int i = 0; i < c->n_tools; i++)
        if (strcmp(c->tool_id[i], id) == 0) return i;
    if (c->n_tools >= GK_TOOL_CAP) return -1;
    int i = c->n_tools++;
    snprintf(c->tool_id[i], sizeof c->tool_id[i], "%s", id);
    c->tool_flags[i] = 0;
    c->tool_text[i] = NULL;
    c->tool_name[i][0] = '\0';
    return i;
}

static void gk_lower_copy(char *dst, size_t n, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) {
        char ch = src[i];
        dst[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
    }
    dst[i] = '\0';
}

static int gk_istarts(const char *s, const char *prefix)
{
    if (!s) return 0;
    for (; *prefix; s++, prefix++) {
        char a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (!a || a != b) return 0;
    }
    return 1;
}

/* ACP kind "search" is shared by workspace grep and Grok's web search.
 * The variant and title tell them apart; kind is only a fallback. */
static const char *gk_tool_name(cJSON *u, char *scratch, size_t n) {
    cJSON *raw = cJSON_GetObjectItem(u, "rawInput");
    const char *variant = cJSON_GetStringValue(cJSON_GetObjectItem(raw, "variant"));
    if (variant && *variant) {
        if (!strcmp(variant, "WebSearch")) return "web";
        if (!strcmp(variant, "Grep"))      return "grep";
        if (!strcmp(variant, "WebFetch"))  return "web_fetch";
        if (!strcmp(variant, "ReadFile"))  return "read";
        if (!strcmp(variant, "ListDir"))   return "ls";
        if (!strcmp(variant, "Bash"))      return "bash";
    }
    const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(u, "title"));
    if (title && gk_istarts(title, "web search")) return "web";

    cJSON *meta = cJSON_GetObjectItem(u, "_meta");
    cJSON *tool = meta ? cJSON_GetObjectItem(meta, "x.ai/tool") : NULL;
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(u, "kind"));
    if ((!kind || !*kind) && cJSON_IsObject(tool))
        kind = cJSON_GetStringValue(cJSON_GetObjectItem(tool, "kind"));
    if (kind && *kind) {
        if (!strcmp(kind, "execute")) return "bash";
        if (!strcmp(kind, "search"))  return "grep";
        if (!strcmp(kind, "fetch"))   return "web_fetch";
        return kind;
    }
    if (cJSON_IsString(tool) && tool->valuestring[0]) {
        gk_lower_copy(scratch, n, tool->valuestring);
        return scratch;
    }
    if (cJSON_IsObject(tool)) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(tool, "name"));
        if (!name) name = cJSON_GetStringValue(cJSON_GetObjectItem(tool, "label"));
        if (name && *name) {
            gk_lower_copy(scratch, n, name);
            for (char *p = scratch; *p; p++)
                if (*p == ' ') *p = '_';
            if (!strcmp(scratch, "run_command") || !strcmp(scratch, "run_terminal_command"))
                return "bash";
            if (!strcmp(scratch, "list_files") || !strcmp(scratch, "list_dir"))
                return "ls";
            if (!strcmp(scratch, "web_search") || !strcmp(scratch, "websearch"))
                return "web";
            return scratch;
        }
    }
    if (title && *title) {
        gk_lower_copy(scratch, n, title);
        char *sp = strchr(scratch, ' ');
        if (sp) *sp = '\0';
        return scratch[0] ? scratch : "tool";
    }
    return "tool";
}

static void gk_concat(char **dst, const char *src) {
    if (!src || !*src) return;
    size_t al = *dst ? strlen(*dst) : 0, tl = strlen(src);
    char *n = realloc(*dst, al + tl + 1);
    if (!n) return;
    memcpy(n + al, src, tl + 1);
    *dst = n;
}

static void gk_append(char **dst, const char *src) {
    if (!src || !*src) return;
    size_t al = *dst ? strlen(*dst) : 0;
    if (al && (*dst)[al - 1] != '\n') gk_concat(dst, "\n");
    gk_concat(dst, src);
}

/* Flatten a tool_call content[] array into plain text. Diff items contribute
 * only their path; a failed result skips them so a path is not mistaken for
 * the error. */
static char *gk_content_text(cJSON *content, int paths) {
    if (!cJSON_IsArray(content)) return NULL;
    char *out = NULL;
    cJSON *item;
    cJSON_ArrayForEach(item, content) {
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(item, "type"));
        if (type && !strcmp(type, "content")) {
            cJSON *inner = cJSON_GetObjectItem(item, "content");
            const char *txt = inner ? cJSON_GetStringValue(cJSON_GetObjectItem(inner, "text")) : NULL;
            gk_append(&out, txt);
        } else if (type && !strcmp(type, "text")) {
            gk_append(&out, cJSON_GetStringValue(cJSON_GetObjectItem(item, "text")));
        } else if (paths && type && !strcmp(type, "diff")) {
            const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(item, "path"));
            if (path) gk_append(&out, path);
        }
    }
    return out;
}

static const char *gk_str(cJSON *obj, const char *key) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(obj, key));
    return (s && *s) ? s : NULL;
}

/* A string, or the message/error/text of an object. */
static char *gk_dup_reason(cJSON *v) {
    if (!v) return NULL;
    if (cJSON_IsString(v) && v->valuestring && *v->valuestring)
        return strdup(v->valuestring);
    if (!cJSON_IsObject(v)) return NULL;
    const char *s = gk_str(v, "message");
    if (!s) s = gk_str(v, "error");
    if (!s) s = gk_str(v, "text");
    return s ? strdup(s) : NULL;
}

/* Grok parks the reason on rawOutput.message, or under a variant key
 * ({"type":"NotFound","NotFound":"…"} / FileReadError:{message}). */
static char *gk_mine_reason(cJSON *raw) {
    char *got = gk_dup_reason(raw);
    if (got) return got;
    if (!cJSON_IsObject(raw)) return NULL;

    static const char *keys[] = {
        "message", "error", "output_for_prompt", "output", "content", "text", "result", NULL
    };
    for (int i = 0; keys[i]; i++) {
        got = gk_dup_reason(cJSON_GetObjectItem(raw, keys[i]));
        if (got) return got;
    }
    cJSON *child;
    cJSON_ArrayForEach(child, raw) {
        if (!child->string || !strcmp(child->string, "type")) continue;
        got = gk_dup_reason(child);
        if (got) return got;
    }
    /* A variant with only its discriminant still names the failure. */
    const char *typ = gk_str(raw, "type");
    return typ ? strdup(typ) : NULL;
}

static char *gk_raw_text(cJSON *raw) {
    if (!raw) return NULL;
    char *mined = gk_mine_reason(raw);
    if (mined) return mined;
    if (cJSON_IsString(raw)) return strdup(raw->valuestring ? raw->valuestring : "");
    return cJSON_PrintUnformatted(raw);
}

/* Text this update actually carries — never the word "failed", and never a
 * diff path. A later status-only packet must not wipe an earlier reason. */
static char *gk_extract_reason(cJSON *u) {
    char *text = gk_content_text(cJSON_GetObjectItem(u, "content"), 0);
    if (text && *text) return text;
    free(text);
    text = gk_raw_text(cJSON_GetObjectItem(u, "rawOutput"));
    if (text && *text) return text;
    free(text);

    cJSON *meta = cJSON_GetObjectItem(u, "_meta");
    cJSON *tool = meta ? cJSON_GetObjectItem(meta, "x.ai/tool") : NULL;
    text = gk_dup_reason(tool);
    if (text && *text) return text;
    free(text);

    const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(u, "title"));
    if (title && strchr(title, ' ')) return strdup(title);
    return NULL;
}

static void gk_keep_reason(grok_client *c, int slot, char *text) {
    if (slot < 0 || !text || !*text) {
        free(text);
        return;
    }
    free(c->tool_text[slot]);
    c->tool_text[slot] = text;
}

static const char *const GK_ARG_KEYS[] = {
    "command", "file_path", "target_file", "path", "target_directory", "directory",
    "pattern", "url", "query", "prompt", "description", "notebook_path", NULL
};

static const char *gk_first_string(cJSON *obj, const char *const *keys)
{
    if (!cJSON_IsObject(obj)) return NULL;
    for (int i = 0; keys[i]; i++) {
        const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(obj, keys[i]));
        if (s && *s) return s;
    }
    return NULL;
}

static char *gk_wrap_kv(const char *key, const char *value)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, key, value);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

static int gk_input_useful(const char *json)
{
    if (!json || !*json) return 0;
    cJSON *obj = cJSON_Parse(json);
    int ok = gk_first_string(obj, GK_ARG_KEYS) != NULL;
    cJSON_Delete(obj);
    return ok;
}

/* Web search arrives as rawInput {variant, backend} with no query; the
 * query is on the completed update as rawOutput.action.query. A packet
 * that is only a variant tag is not worth announcing. */
static char *gk_input_json(cJSON *u) {
    cJSON *raw_out = cJSON_GetObjectItem(u, "rawOutput");
    cJSON *action = cJSON_IsObject(raw_out) ? cJSON_GetObjectItem(raw_out, "action") : NULL;
    const char *q = cJSON_GetStringValue(cJSON_GetObjectItem(action, "query"));
    if (!q) q = cJSON_GetStringValue(cJSON_GetObjectItem(raw_out, "query"));
    if (q && *q) return gk_wrap_kv("query", q);

    cJSON *meta = cJSON_GetObjectItem(u, "_meta");
    cJSON *tool = meta ? cJSON_GetObjectItem(meta, "x.ai/tool") : NULL;
    cJSON *tin = cJSON_IsObject(tool) ? cJSON_GetObjectItem(tool, "input") : NULL;
    if (cJSON_IsObject(tin) && gk_first_string(tin, GK_ARG_KEYS))
        return cJSON_PrintUnformatted(tin);

    cJSON *raw = cJSON_GetObjectItem(u, "rawInput");
    if (cJSON_IsObject(raw) && gk_first_string(raw, GK_ARG_KEYS))
        return cJSON_PrintUnformatted(raw);
    if (raw && (cJSON_IsArray(raw) || cJSON_IsString(raw)))
        return cJSON_PrintUnformatted(raw);

    const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(u, "title"));
    if (title && gk_istarts(title, "web search")) {
        const char *colon = strchr(title, ':');
        if (colon) {
            const char *p = colon + 1;
            while (*p == ' ') p++;
            if (*p) return gk_wrap_kv("query", p);
        }
    }

    cJSON *locs = cJSON_GetObjectItem(u, "locations");
    if (cJSON_IsArray(locs) && cJSON_GetArraySize(locs) > 0) {
        const char *path = cJSON_GetStringValue(
            cJSON_GetObjectItem(cJSON_GetArrayItem(locs, 0), "path"));
        if (path && *path) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "path", path);
            char *json = cJSON_PrintUnformatted(obj);
            cJSON_Delete(obj);
            return json;
        }
    }
    return NULL;
}

static void gk_keep_name(grok_client *c, int slot, const char *name)
{
    if (slot < 0 || !name || !*name) return;
    if (!c->tool_name[slot][0] || !strcmp(c->tool_name[slot], "tool"))
        snprintf(c->tool_name[slot], sizeof c->tool_name[slot], "%s", name);
    else if (!strcmp(c->tool_name[slot], "grep") && !strcmp(name, "web"))
        snprintf(c->tool_name[slot], sizeof c->tool_name[slot], "%s", name);
}

/* Title is a last-resort argument. The generic "Web search:" label is not one. */
static const char *gk_title_arg(const char *title, const char *name)
{
    if (!title || !*title) return NULL;
    if (name && !strcmp(title, name)) return NULL;
    if (gk_istarts(title, "web search")) {
        const char *colon = strchr(title, ':');
        if (!colon) return NULL;
        const char *p = colon + 1;
        while (*p == ' ') p++;
        return *p ? p : NULL;
    }
    return title;
}

static void gk_tool_update(grok_client *c, cJSON *u) {
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(u, "toolCallId"));
    int slot = gk_tool_slot(c, id);
    char namebuf[64];
    const char *name = gk_tool_name(u, namebuf, sizeof namebuf);
    const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(u, "title"));
    const char *status = cJSON_GetStringValue(cJSON_GetObjectItem(u, "status"));
    char *input = gk_input_json(u);
    gk_keep_name(c, slot, name);
    if (slot >= 0 && c->tool_name[slot][0])
        name = c->tool_name[slot];
    int have_shape = (cJSON_GetObjectItem(u, "kind") || title ||
                      cJSON_GetObjectItem(u, "rawInput") ||
                      cJSON_GetObjectItem(u, "_meta"));
    int useful = gk_input_useful(input);
    int failed = status && !strcmp(status, "failed");
    int done = failed || (status && !strcmp(status, "completed"));

    /* Web search only names the query on the completed update. */
    if (slot >= 0 && !(c->tool_flags[slot] & 1) && have_shape && (useful || done)) {
        c->tool_flags[slot] |= 1;
        grok_event ev = {
            .kind = GROK_EV_TOOL,
            .name = name,
            .input_json = input,
            .text = useful ? NULL : gk_title_arg(title, name),
        };
        gk_emit(c, &ev);
    }

    gk_keep_reason(c, slot, gk_extract_reason(u));

    if (slot >= 0 && done && !(c->tool_flags[slot] & 2)) {
        c->tool_flags[slot] |= 2;
        char *owned = NULL;
        const char *text = c->tool_text[slot];
        if (!failed && (!text || !*text)) {
            owned = gk_content_text(cJSON_GetObjectItem(u, "content"), 1);
            if (!owned) owned = gk_raw_text(cJSON_GetObjectItem(u, "rawOutput"));
            text = owned;
        }
        if (!text || !*text) text = failed ? "failed" : "";
        grok_event ev = {
            .kind = GROK_EV_TOOL_RESULT,
            .text = text,
            .failed = failed,
        };
        gk_emit(c, &ev);
        free(owned);
    }
    free(input);
}

/* Answer an agent->client request so the turn never blocks. Permission
 * requests are approved (or cancelled, if we already sent session/cancel). */
static void gk_answer_request(grok_client *c, cJSON *req, cJSON *id) {
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(req, "method"));
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    cJSON *result = cJSON_AddObjectToObject(resp, "result");
    if (method && strcmp(method, "session/request_permission") == 0) {
        cJSON *outcome = cJSON_AddObjectToObject(result, "outcome");
        if (c->cancelling) {
            cJSON_AddStringToObject(outcome, "outcome", "cancelled");
        } else {
            cJSON *params = cJSON_GetObjectItem(req, "params");
            cJSON *opts = params ? cJSON_GetObjectItem(params, "options") : NULL;
            const char *pick = NULL;
            cJSON *o;
            cJSON_ArrayForEach(o, opts) {
                const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(o, "kind"));
                const char *oid  = cJSON_GetStringValue(cJSON_GetObjectItem(o, "optionId"));
                if (!oid) continue;
                if (!pick) pick = oid;
                if (kind && strncmp(kind, "allow", 5) == 0) { pick = oid; break; }
            }
            cJSON_AddStringToObject(outcome, "outcome", "selected");
            cJSON_AddStringToObject(outcome, "optionId", pick ? pick : "allow");
        }
    }
    gk_write(c, resp);
}

/* Handle one parsed event line during a wait for response `want_id`.
 * Accumulates agent_message_chunk text into *acc (if non-NULL). Returns:
 *   1  -> response for want_id arrived; *ok set if the result is usable
 *   0  -> not the awaited response (keep reading)                              */
static int gk_handle(grok_client *c, cJSON *ev, int want_id, char **acc, int *ok) {
    cJSON *idj = cJSON_GetObjectItem(ev, "id");
    cJSON *method = cJSON_GetObjectItem(ev, "method");

    if (method && cJSON_IsString(method)) {
        if (idj) { gk_answer_request(c, ev, idj); return 0; }
        if (strcmp(method->valuestring, "session/update") == 0) {
            cJSON *params = cJSON_GetObjectItem(ev, "params");
            cJSON *u = params ? cJSON_GetObjectItem(params, "update") : NULL;
            const char *su = u ? cJSON_GetStringValue(cJSON_GetObjectItem(u, "sessionUpdate")) : NULL;
            if (su) {
                cJSON *content = cJSON_GetObjectItem(u, "content");
                const char *txt = content ? cJSON_GetStringValue(cJSON_GetObjectItem(content, "text")) : NULL;
                if (strcmp(su, "agent_message_chunk") == 0 && txt) {
                    if (acc) gk_concat(acc, txt);
                    grok_event e = { .kind = GROK_EV_ASSISTANT, .text = txt };
                    gk_emit(c, &e);
                } else if (strcmp(su, "agent_thought_chunk") == 0 && txt) {
                    grok_event e = { .kind = GROK_EV_THINKING, .text = txt };
                    gk_emit(c, &e);
                } else if (!strcmp(su, "tool_call") || !strcmp(su, "tool_call_update")) {
                    gk_tool_update(c, u);
                }
            }
        }
        return 0;
    }

    /* response to one of our requests */
    if (idj && cJSON_IsNumber(idj) && (int)cJSON_GetNumberValue(idj) == want_id) {
        cJSON *err = cJSON_GetObjectItem(ev, "error");
        cJSON *res = cJSON_GetObjectItem(ev, "result");
        *ok = res != NULL;
        if (!*ok && err) {
            const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(err, "message"));
            /* A cancel can surface as an RPC error; the turn still ended cleanly. */
            if (c->cancelling) {
                *ok = 1;
            } else if (m) {
                fprintf(stderr, "grok: rpc error: %s\n", m);
            }
        }
        if (*ok && res) {
            const char *stop = cJSON_GetStringValue(cJSON_GetObjectItem(res, "stopReason"));
            if (stop && !strcmp(stop, "cancelled") && c->meta)
                c->meta->interrupted = 1;
        }
        return 1;
    }
    return 0;
}

/* Read/parse lines until the response to `want_id`. On session/new we need the
 * result body, so `sid_out` (if non-NULL) is filled from result.sessionId.
 * Accumulates assistant text into *acc when non-NULL. Returns 1 on a successful
 * result, 0 on error/EOF. `allow_cancel` sends session/cancel the first time
 * the abort predicate fires (or was latched during the handshake). */
static int gk_await(grok_client *c, int want_id, char **acc,
                    char *sid_out, size_t sid_sz, int allow_cancel) {
    char tmp[8192];
    int sent_cancel = 0;
    for (;;) {
        if (c->abort && c->abort())
            c->abort_latched = 1;
        if (allow_cancel && c->abort_latched && !sent_cancel && c->session_id[0]) {
            sent_cancel = 1;
            if (c->meta) c->meta->interrupted = 1;
            grok_interrupt(c);
        }
        struct pollfd pfd = { c->out_fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, GK_TICK_MS);
        if (pr < 0) { if (errno == EINTR) continue; c->len = 0; return 0; }
        if (pr == 0) continue;
        ssize_t r = read(c->out_fd, tmp, sizeof tmp);
        if (r <= 0) { c->len = 0; return 0; }

        if (c->len + (size_t)r + 1 > c->cap) {
            size_t nc = (c->len + (size_t)r + 1) * 2;
            char *nb = realloc(c->buf, nc);
            if (!nb) { c->len = 0; return 0; }
            c->buf = nb; c->cap = nc;
        }
        memcpy(c->buf + c->len, tmp, (size_t)r);
        c->len += (size_t)r;
        c->buf[c->len] = '\0';

        char *start = c->buf, *nl;
        while ((nl = memchr(start, '\n', c->len - (size_t)(start - c->buf)))) {
            *nl = '\0';
            cJSON *ev = cJSON_Parse(start);
            int done = 0, ok = 0;
            if (ev) {
                done = gk_handle(c, ev, want_id, acc, &ok);
                if (done && ok && sid_out) {
                    cJSON *res = cJSON_GetObjectItem(ev, "result");
                    const char *sid = res ? cJSON_GetStringValue(cJSON_GetObjectItem(res, "sessionId")) : NULL;
                    if (sid) snprintf(sid_out, sid_sz, "%s", sid);
                }
                cJSON_Delete(ev);
            }
            if (done) {
                size_t consumed = (size_t)(nl + 1 - c->buf);
                memmove(c->buf, c->buf + consumed, c->len - consumed);
                c->len -= consumed;
                return ok;
            }
            start = nl + 1;
        }
        size_t consumed = (size_t)(start - c->buf);
        if (consumed) { memmove(c->buf, start, c->len - consumed); c->len -= consumed; }
    }
}

grok_client *grok_start(const grok_opts *opts) {
    grok_opts o = opts ? *opts : (grok_opts){0};
    const char *cli = (o.cli_path && *o.cli_path) ? o.cli_path : "grok";

    signal(SIGPIPE, SIG_IGN);

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0) return NULL;
    if (pipe(out_pipe) != 0) { close(in_pipe[0]); close(in_pipe[1]); return NULL; }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return NULL;
    }
    if (pid == 0) {
        if (dup2(in_pipe[0], STDIN_FILENO) < 0)   _exit(126);
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0) _exit(126);
        if (in_pipe[0]  != STDIN_FILENO)  close(in_pipe[0]);
        if (in_pipe[1]  != STDIN_FILENO)  close(in_pipe[1]);
        if (out_pipe[0] != STDOUT_FILENO) close(out_pipe[0]);
        if (out_pipe[1] != STDOUT_FILENO) close(out_pipe[1]);
        if (o.cwd && *o.cwd) { if (chdir(o.cwd) != 0) _exit(126); }

        const char *argv[16];
        int n = 0;
        argv[n++] = cli;
        argv[n++] = "agent";
        if (o.model && *o.model)                     { argv[n++] = "-m"; argv[n++] = o.model; }
        if (o.reasoning_effort && *o.reasoning_effort) { argv[n++] = "--reasoning-effort"; argv[n++] = o.reasoning_effort; }
        argv[n++] = "--always-approve";
        argv[n++] = "stdio";
        argv[n] = NULL;
        execvp(cli, (char *const *)argv);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    fcntl(in_pipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
    grok_client *c = calloc(1, sizeof *c);
    if (!c) {
        close(in_pipe[1]); close(out_pipe[0]);
        kill(pid, SIGKILL); waitpid(pid, NULL, 0);
        return NULL;
    }
    c->pid = pid;
    c->in_fd = in_pipe[1];
    c->out_fd = out_pipe[0];
    c->next_id = 1;
    c->cwd = (o.cwd && *o.cwd) ? strdup(o.cwd) : NULL;
    c->sys = (o.append_system && *o.append_system) ? strdup(o.append_system) : NULL;
    c->resume = (o.resume_session && *o.resume_session) ? strdup(o.resume_session) : NULL;
    c->no_session = o.no_session;

    return c;
}

/* The ACP handshake, deferred to the first turn: it blocks until the CLI has
 * booted, which is over a second of Node startup that an interactive caller
 * would otherwise wait out before it could show anything. Returns nonzero once
 * the session is live. */
static int gk_handshake(grok_client *c) {
    if (c->session_id[0]) return 1;
    if (c->handshake_failed) return 0;
    c->handshake_failed = 1;

    cJSON *init = cJSON_CreateObject();
    cJSON_AddStringToObject(init, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(init, "id", c->next_id);
    cJSON_AddStringToObject(init, "method", "initialize");
    cJSON *ip = cJSON_AddObjectToObject(init, "params");
    cJSON_AddNumberToObject(ip, "protocolVersion", 1);
    cJSON_AddObjectToObject(ip, "clientCapabilities");
    cJSON *info = cJSON_AddObjectToObject(ip, "clientInfo");
    cJSON_AddStringToObject(info, "name", "grok.h");
    cJSON_AddStringToObject(info, "version", "1");
    if (!gk_write(c, init) || !gk_await(c, c->next_id, NULL, NULL, 0, 0)) return 0;
    c->next_id++;

    /* session/resume keeps prior context without replaying the transcript into
     * the event stream, which is what this driver wants on a restart. */
    const char *resume = (c->resume && *c->resume) ? c->resume : NULL;
    cJSON *sn = cJSON_CreateObject();
    cJSON_AddStringToObject(sn, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(sn, "id", c->next_id);
    cJSON_AddStringToObject(sn, "method", resume ? "session/resume" : "session/new");
    cJSON *sp = cJSON_AddObjectToObject(sn, "params");
    if (resume)
        cJSON_AddStringToObject(sp, "sessionId", resume);
    cJSON_AddStringToObject(sp, "cwd", c->cwd ? c->cwd : "/");
    cJSON_AddItemToObject(sp, "mcpServers", cJSON_CreateArray());
    cJSON *meta = cJSON_AddObjectToObject(sp, "_meta");
    cJSON_AddBoolToObject(meta, "yoloMode", 1);
    if (c->no_session)
        cJSON_AddBoolToObject(meta, "x.ai/persist", 0);
    if (!gk_write(c, sn) || !gk_await(c, c->next_id, NULL, c->session_id, sizeof c->session_id, 0))
        return 0;
    c->next_id++;
    if (resume && !c->session_id[0])
        snprintf(c->session_id, sizeof c->session_id, "%s", resume);

    c->handshake_failed = 0;
    return 1;
}

int grok_connect(grok_client *c) {
    return c && gk_handshake(c);
}

char *grok_send_ex(grok_client *c, const char *user_text, grok_result *meta) {
    if (meta) memset(meta, 0, sizeof *meta);
    if (!c || !user_text || !gk_handshake(c)) return NULL;
    /* ESC during the handshake is latched so it is not lost, but there is no
     * prompt to cancel yet. Treat it as an interrupted empty turn. */
    if (c->abort_latched) {
        c->abort_latched = 0;
        if (meta) meta->interrupted = 1;
        return strdup("");
    }

    /* Prepend the system prompt to each turn (ACP has no system field). */
    char *full = NULL;
    if (c->sys) {
        size_t need = strlen(c->sys) + strlen(user_text) + 8;
        full = malloc(need);
        if (!full) return NULL;
        snprintf(full, need, "%s\n\n---\n\n%s", c->sys, user_text);
    }
    const char *payload = full ? full : user_text;

    int id = c->next_id++;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", id);
    cJSON_AddStringToObject(req, "method", "session/prompt");
    cJSON *p = cJSON_AddObjectToObject(req, "params");
    cJSON_AddStringToObject(p, "sessionId", c->session_id);
    cJSON *arr = cJSON_AddArrayToObject(p, "prompt");
    cJSON *block = cJSON_CreateObject();
    cJSON_AddStringToObject(block, "type", "text");
    cJSON_AddStringToObject(block, "text", payload);
    cJSON_AddItemToArray(arr, block);
    int wrote = gk_write(c, req);
    free(full);
    if (!wrote) return NULL;

    c->meta = meta;
    c->cancelling = 0;
    gk_reset_tools(c);
    char *acc = NULL;
    int ok = gk_await(c, id, &acc, NULL, 0, 1);
    c->meta = NULL;
    c->cancelling = 0;
    c->abort_latched = 0;
    if (!ok) { free(acc); return NULL; }
    return acc ? acc : strdup("");
}

char *grok_send(grok_client *c, const char *user_text) {
    return grok_send_ex(c, user_text, NULL);
}

/* Poll for the child's exit for up to `ms`. Returns nonzero once reaped. */
static int gk_reap_within(grok_client *c, int ms) {
    for (int waited = 0; waited < ms; waited += 5) {
        if (waitpid(c->pid, NULL, WNOHANG) == c->pid) { c->pid = 0; return 1; }
        usleep(5000);
    }
    return 0;
}

void grok_stop(grok_client *c) {
    if (!c) return;
    if (c->in_fd >= 0) close(c->in_fd);     /* EOF on stdin asks it to exit */
    if (c->pid > 0) {
        /* We only stop between turns, so the CLI has nothing left to finish.
         * Waiting out the Node runtime's unwind costs about a second, which an
         * interactive caller feels on quit, so escalate immediately. */
        kill(c->pid, SIGTERM);
        if (!gk_reap_within(c, 50)) {
            kill(c->pid, SIGKILL);
            waitpid(c->pid, NULL, 0);
            c->pid = 0;
        }
    }
    if (c->out_fd >= 0) close(c->out_fd);
    gk_reset_tools(c);
    free(c->cwd);
    free(c->sys);
    free(c->resume);
    free(c->buf);
    free(c);
}

#endif /* GROK_IMPLEMENTATION */
