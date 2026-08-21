/**
 * telegram.h - Single-header, stb-style Telegram Bot API client (C)
 *
 * A tiny synchronous client for the Telegram Bot API over libcurl. It covers the
 * pieces a personal bot needs: long-poll for updates, send a text reply, and
 * download a file (photo / document) a user sent. No threads — each call blocks
 * until the HTTP request completes, which is exactly what a poll loop wants.
 * cJSON builds/parses the JSON.
 *
 * Sibling of anthropic.h / llm.h; same packaging conventions.
 *
 * USAGE (stb style):
 *
 *   // In exactly ONE .c file:
 *   #define TELEGRAM_IMPLEMENTATION
 *   #include "telegram.h"
 *
 *   // In every other file that needs the API:
 *   #include "telegram.h"
 *
 * DROP-IN FILES: telegram.h + cJSON.h + cJSON.c (cJSON stays as sibling files;
 * it is NOT amalgamated into this header). Compile cJSON.c into your build.
 *
 * LINK: -lcurl
 * TOKEN: pass the bot token (from @BotFather) to tg_new(). Load it from the
 *        environment; never hard-code or commit it.
 *
 * MINIMAL POLL LOOP:
 *
 *   tg_client *c = tg_new(getenv("TELEGRAM_TOKEN"));
 *   long offset = 0;
 *   for (;;) {
 *       tg_update *u = NULL;
 *       int n = tg_get_updates(c, offset, 30, &u);   // blocks up to ~30s
 *       for (int i = 0; i < n; i++) {
 *           if (u[i].text) tg_send_message(c, u[i].chat_id, u[i].text); // echo
 *           offset = u[i].update_id + 1;              // ack: never re-deliver
 *       }
 *       tg_updates_free(u, n);
 *   }
 *
 * Only ONE process may long-poll a given bot at a time: getUpdates acks by
 * offset, so a second poller silently steals half the messages.
 */

#ifndef TELEGRAM_H
#define TELEGRAM_H

#include <stddef.h>

typedef struct tg_client tg_client;

/* One inbound message, flattened. Optional fields are NULL when absent. All
 * strings are malloc'd and freed by tg_updates_free(). */
typedef struct {
    long  update_id;    /* monotonic; pass max+1 back as the next offset */
    long  chat_id;      /* where to reply (tg_send_message) */
    long  from_id;      /* sender's user id (for an allow-list) */
    char *text;         /* message text / caption, or NULL */
    char *file_id;      /* photo (largest size) or document id, else NULL */
    char *file_name;    /* document filename, else NULL */
    char *mime_type;    /* document/photo mime type, else NULL */
    char *media_group_id; /* set (and shared) when several files are sent as one
                             album; NULL for a standalone message */
} tg_update;

/* Create/destroy a client. tg_new returns NULL if token is NULL/empty or on
 * allocation/curl-init failure. */
tg_client *tg_new(const char *token);
void       tg_free(tg_client *c);

/* Register a callback polled (~1x/sec) during blocking requests; returning
 * nonzero aborts the in-flight request. Use it to break out of a long-poll on
 * shutdown so Ctrl-C takes effect promptly. Pass NULL to clear. */
void       tg_set_abort_check(int (*cb)(void));

/* Where this client's diagnostics go. The default is stderr, which is wrong for
 * a caller that owns the screen: the messages come off a polling thread and
 * would land in the middle of whatever is drawn. Pass NULL to restore it. */
void       tg_set_log(void (*fn)(const char *msg));

/*
 * Long-poll for updates. Blocks up to timeout_s seconds server-side (0 = return
 * immediately). `offset` is the first update_id you have NOT yet processed
 * (last seen + 1); 0 means "give me whatever is pending". On success returns the
 * number of updates (>= 0) and sets *out to a malloc'd array of that many
 * tg_update (NULL when 0). Free with tg_updates_free(). Returns -1 on error
 * (message printed to stderr) with *out set to NULL.
 *
 * Only "message" updates are requested; other update types are skipped but
 * still advance the offset so they don't wedge the loop.
 */
int  tg_get_updates(tg_client *c, long offset, int timeout_s, tg_update **out);
void tg_updates_free(tg_update *updates, int n);

/* Send a plain-text message. Returns 0 on success, -1 on failure. */
int  tg_send_message(tg_client *c, long chat_id, const char *text);

/* Send `text`, which must already be MarkdownV2 (see mdv2.h), with parse_mode
 * set. Telegram rejects the whole message if an entity is malformed, so on
 * failure the text is unescaped and re-sent unformatted rather than dropped.
 * Returns 0 when it rendered as markdown, 1 when it fell back to plain text,
 * and -1 when neither send succeeded. */
int  tg_send_message_md(tg_client *c, long chat_id, const char *text);

/* Show a chat action (e.g. "typing") in the chat. Telegram displays it for ~5s
 * or until the next message. Returns 0 on success, -1 on failure. */
int  tg_send_chat_action(tg_client *c, long chat_id, const char *action);

/* Upload a local file and send it to the chat. tg_send_photo displays images
 * inline (sendPhoto, ~10 MB cap); tg_send_document sends any file as an
 * attachment (sendDocument, ~50 MB cap). `caption` may be NULL. Multipart
 * upload. Returns 0 on success, -1 on failure. */
int  tg_send_photo(tg_client *c, long chat_id, const char *file_path, const char *caption);
int  tg_send_document(tg_client *c, long chat_id, const char *file_path, const char *caption);

/*
 * Download the file identified by file_id to dest_path (overwritten). Two hops:
 * getFile -> file_path, then GET the file endpoint. Bot API caps downloads at
 * 20 MB. Returns 0 on success, -1 on failure.
 */
int  tg_download_file(tg_client *c, const char *file_id, const char *dest_path);

#endif /* TELEGRAM_H */

/* ======================================================================== */
/*   IMPLEMENTATION                                                          */
/* ======================================================================== */
#ifdef TELEGRAM_IMPLEMENTATION

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "cJSON.h"

#ifndef TG_API_BASE
#define TG_API_BASE  "https://api.telegram.org"
#endif

struct tg_client {
    char *token;
    CURL *curl;   /* reused across calls (single-threaded); reset each time */
};

static int (*tg_abort_check)(void) = NULL;
void tg_set_abort_check(int (*cb)(void)) { tg_abort_check = cb; }

static void (*tg_log_fn)(const char *msg) = NULL;
void tg_set_log(void (*fn)(const char *msg)) { tg_log_fn = fn; }

/* Every diagnostic in this header goes through here. */
static void tg_logf(const char *fmt, ...) {
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (tg_log_fn) tg_log_fn(line);
    else fprintf(stderr, "telegram: %s\n", line);
}

/* libcurl progress callback: abort the transfer when the caller signals stop. */
static int tg_xferinfo(void *p, curl_off_t dt, curl_off_t dn,
                       curl_off_t ut, curl_off_t un) {
    (void)p; (void)dt; (void)dn; (void)ut; (void)un;
    return (tg_abort_check && tg_abort_check()) ? 1 : 0;
}

/* Growable response buffer for the memory write callback. */
typedef struct { char *p; size_t len; } tg_buf;

static size_t tg_on_data(char *ptr, size_t size, size_t nmemb, void *ud) {
    size_t n = size * nmemb;
    tg_buf *b = (tg_buf *)ud;
    char *np = realloc(b->p, b->len + n + 1);
    if (!np) return 0;               /* signal error to libcurl */
    b->p = np;
    memcpy(b->p + b->len, ptr, n);
    b->len += n;
    b->p[b->len] = '\0';
    return n;
}

static size_t tg_on_file(char *ptr, size_t size, size_t nmemb, void *ud) {
    return fwrite(ptr, size, nmemb, (FILE *)ud);
}

tg_client *tg_new(const char *token) {
    if (!token || !*token) {
        tg_logf("tg_new called with empty token");
        return NULL;
    }
    tg_client *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->token = strdup(token);
    c->curl  = curl_easy_init();
    if (!c->token || !c->curl) { tg_free(c); return NULL; }
    return c;
}

void tg_free(tg_client *c) {
    if (!c) return;
    if (c->curl) curl_easy_cleanup(c->curl);
    free(c->token);
    free(c);
}

/*
 * POST a JSON body to a bot method and return the parsed response root, or NULL
 * on transport/parse error or when the API reports ok:false (logged). Caller
 * cJSON_Delete's the result.
 */
static cJSON *tg_post(tg_client *c, const char *method, const char *body) {
    char url[512];
    snprintf(url, sizeof url, TG_API_BASE "/bot%s/%s", c->token, method);

    struct curl_slist *hdr =
        curl_slist_append(NULL, "content-type: application/json");
    tg_buf buf = {0};

    curl_easy_reset(c->curl);
    curl_easy_setopt(c->curl, CURLOPT_URL, url);
    curl_easy_setopt(c->curl, CURLOPT_POST, 1L);
    curl_easy_setopt(c->curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c->curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(c->curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c->curl, CURLOPT_WRITEFUNCTION, tg_on_data);
    curl_easy_setopt(c->curl, CURLOPT_WRITEDATA, &buf);
    /* getUpdates holds the connection open for its own timeout; allow slack. */
    curl_easy_setopt(c->curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c->curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c->curl, CURLOPT_XFERINFOFUNCTION, tg_xferinfo);

    CURLcode rc = curl_easy_perform(c->curl);
    curl_slist_free_all(hdr);
    if (rc != CURLE_OK) {
        tg_logf("%s: %s", method, curl_easy_strerror(rc));
        free(buf.p);
        return NULL;
    }

    cJSON *root = cJSON_Parse(buf.p ? buf.p : "");
    free(buf.p);
    if (!root) {
        tg_logf("%s: could not parse response", method);
        return NULL;
    }
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsBool(ok) || !cJSON_IsTrue(ok)) {
        cJSON *desc = cJSON_GetObjectItemCaseSensitive(root, "description");
        tg_logf("%s failed: %s", method,
                (desc && cJSON_IsString(desc)) ? desc->valuestring : "unknown");
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static char *tg_strdup_or_null(const cJSON *s) {
    return (s && cJSON_IsString(s) && s->valuestring) ? strdup(s->valuestring)
                                                      : NULL;
}

/* Pull the fields we care about out of one Telegram "message" object. */
static void tg_parse_message(const cJSON *msg, tg_update *u) {
    cJSON *chat = cJSON_GetObjectItemCaseSensitive(msg, "chat");
    cJSON *chat_id = chat ? cJSON_GetObjectItemCaseSensitive(chat, "id") : NULL;
    if (cJSON_IsNumber(chat_id)) u->chat_id = (long)chat_id->valuedouble;

    cJSON *from = cJSON_GetObjectItemCaseSensitive(msg, "from");
    cJSON *from_id = from ? cJSON_GetObjectItemCaseSensitive(from, "id") : NULL;
    if (cJSON_IsNumber(from_id)) u->from_id = (long)from_id->valuedouble;

    /* text, or a media caption */
    cJSON *text = cJSON_GetObjectItemCaseSensitive(msg, "text");
    if (!text) text = cJSON_GetObjectItemCaseSensitive(msg, "caption");
    u->text = tg_strdup_or_null(text);

    /* Shared across the messages of an album (each photo is its own update). */
    u->media_group_id = tg_strdup_or_null(
        cJSON_GetObjectItemCaseSensitive(msg, "media_group_id"));

    /* A photo arrives as an array of sizes, smallest -> largest. Take the last
     * (highest resolution) file_id. */
    cJSON *photo = cJSON_GetObjectItemCaseSensitive(msg, "photo");
    if (cJSON_IsArray(photo) && cJSON_GetArraySize(photo) > 0) {
        cJSON *largest = cJSON_GetArrayItem(photo, cJSON_GetArraySize(photo) - 1);
        u->file_id = tg_strdup_or_null(
            cJSON_GetObjectItemCaseSensitive(largest, "file_id"));
        u->mime_type = strdup("image/jpeg"); /* Telegram re-encodes photos to JPEG */
        return;
    }

    /* A document (file sent uncompressed) keeps its name + mime type. */
    cJSON *doc = cJSON_GetObjectItemCaseSensitive(msg, "document");
    if (doc) {
        u->file_id   = tg_strdup_or_null(
            cJSON_GetObjectItemCaseSensitive(doc, "file_id"));
        u->file_name = tg_strdup_or_null(
            cJSON_GetObjectItemCaseSensitive(doc, "file_name"));
        u->mime_type = tg_strdup_or_null(
            cJSON_GetObjectItemCaseSensitive(doc, "mime_type"));
        return;
    }

    /* A voice note (or an audio file) — the caller downloads and transcribes it.
     * Telegram sends voice as OGG/OPUS; mime_type defaults to audio/ogg. */
    cJSON *voice = cJSON_GetObjectItemCaseSensitive(msg, "voice");
    if (!voice) voice = cJSON_GetObjectItemCaseSensitive(msg, "audio");
    if (voice) {
        u->file_id   = tg_strdup_or_null(
            cJSON_GetObjectItemCaseSensitive(voice, "file_id"));
        u->file_name = tg_strdup_or_null(
            cJSON_GetObjectItemCaseSensitive(voice, "file_name"));
        u->mime_type = tg_strdup_or_null(
            cJSON_GetObjectItemCaseSensitive(voice, "mime_type"));
        if (!u->mime_type) u->mime_type = strdup("audio/ogg");
    }
}

int tg_get_updates(tg_client *c, long offset, int timeout_s, tg_update **out) {
    *out = NULL;
    if (!c) return -1;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "offset", (double)offset);
    cJSON_AddNumberToObject(req, "timeout", timeout_s);
    cJSON *allowed = cJSON_AddArrayToObject(req, "allowed_updates");
    cJSON_AddItemToArray(allowed, cJSON_CreateString("message"));
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return -1;

    cJSON *root = tg_post(c, "getUpdates", body);
    free(body);
    if (!root) return -1;

    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    int total = cJSON_IsArray(result) ? cJSON_GetArraySize(result) : 0;
    if (total <= 0) { cJSON_Delete(root); return 0; }

    tg_update *arr = calloc((size_t)total, sizeof *arr);
    if (!arr) { cJSON_Delete(root); return -1; }

    int n = 0;
    cJSON *upd;
    cJSON_ArrayForEach(upd, result) {
        cJSON *uid = cJSON_GetObjectItemCaseSensitive(upd, "update_id");
        if (!cJSON_IsNumber(uid)) continue;   /* shouldn't happen */
        tg_update *u = &arr[n];
        u->update_id = (long)uid->valuedouble;
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(upd, "message");
        /* Non-message updates were filtered by allowed_updates, but keep the
         * update_id so the caller still advances the offset past them. */
        if (msg) tg_parse_message(msg, u);
        n++;
    }

    cJSON_Delete(root);
    *out = arr;
    return n;
}

void tg_updates_free(tg_update *updates, int n) {
    if (!updates) return;
    for (int i = 0; i < n; i++) {
        free(updates[i].text);
        free(updates[i].file_id);
        free(updates[i].file_name);
        free(updates[i].mime_type);
        free(updates[i].media_group_id);
    }
    free(updates);
}

int tg_send_message(tg_client *c, long chat_id, const char *text) {
    if (!c || !text) return -1;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "chat_id", (double)chat_id);
    cJSON_AddStringToObject(req, "text", text);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return -1;

    cJSON *root = tg_post(c, "sendMessage", body);
    free(body);
    if (!root) return -1;
    cJSON_Delete(root);
    return 0;
}

int tg_send_message_md(tg_client *c, long chat_id, const char *text) {
    if (!c || !text) return -1;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "chat_id", (double)chat_id);
    cJSON_AddStringToObject(req, "text", text);
    cJSON_AddStringToObject(req, "parse_mode", "MarkdownV2");
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return -1;

    cJSON *root = tg_post(c, "sendMessage", body);
    free(body);
    if (root) { cJSON_Delete(root); return 0; }

    /* Rejected: strip the MarkdownV2 escapes and send it as-is so the content
     * still reaches the chat. */
    size_t n = strlen(text);
    char *plain = malloc(n + 1);
    if (!plain) return -1;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '\\' && i + 1 < n && strchr("_*[]()~`>#+-=|{}.!\\", text[i + 1]))
            i++;
        plain[j++] = text[i];
    }
    plain[j] = '\0';
    int rc = tg_send_message(c, chat_id, plain);
    free(plain);
    return rc == 0 ? 1 : -1;
}

int tg_send_chat_action(tg_client *c, long chat_id, const char *action) {
    if (!c || !action) return -1;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "chat_id", (double)chat_id);
    cJSON_AddStringToObject(req, "action", action);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return -1;

    cJSON *root = tg_post(c, "sendChatAction", body);
    free(body);
    if (!root) return -1;
    cJSON_Delete(root);
    return 0;
}

/* Upload `file_path` via a multipart POST to `method`, with the file under form
 * field `field` (e.g. "photo"/"document") plus chat_id and optional caption. */
static int tg_upload(tg_client *c, const char *method, long chat_id,
                     const char *field, const char *file_path, const char *caption) {
    if (!c || !field || !file_path) return -1;
    char url[512];
    snprintf(url, sizeof url, TG_API_BASE "/bot%s/%s", c->token, method);
    char chat[32];
    snprintf(chat, sizeof chat, "%ld", chat_id);

    curl_easy_reset(c->curl);
    curl_mime *mime = curl_mime_init(c->curl);
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, "chat_id");
    curl_mime_data(part, chat, CURL_ZERO_TERMINATED);
    part = curl_mime_addpart(mime);
    curl_mime_name(part, field);
    curl_mime_filedata(part, file_path);   /* streams the file; sets filename */
    if (caption && *caption) {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "caption");
        curl_mime_data(part, caption, CURL_ZERO_TERMINATED);
    }

    tg_buf buf = {0};
    curl_easy_setopt(c->curl, CURLOPT_URL, url);
    curl_easy_setopt(c->curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(c->curl, CURLOPT_WRITEFUNCTION, tg_on_data);
    curl_easy_setopt(c->curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(c->curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c->curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c->curl, CURLOPT_XFERINFOFUNCTION, tg_xferinfo);

    CURLcode rc = curl_easy_perform(c->curl);
    curl_mime_free(mime);
    if (rc != CURLE_OK) {
        tg_logf("%s: %s", method, curl_easy_strerror(rc));
        free(buf.p);
        return -1;
    }
    cJSON *root = cJSON_Parse(buf.p ? buf.p : "");
    free(buf.p);
    if (!root) { tg_logf("%s: bad response", method); return -1; }
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    int good = cJSON_IsBool(ok) && cJSON_IsTrue(ok);
    if (!good) {
        cJSON *desc = cJSON_GetObjectItemCaseSensitive(root, "description");
        tg_logf("%s failed: %s", method,
                (desc && cJSON_IsString(desc)) ? desc->valuestring : "unknown");
    }
    cJSON_Delete(root);
    return good ? 0 : -1;
}

int tg_send_photo(tg_client *c, long chat_id, const char *file_path, const char *caption) {
    return tg_upload(c, "sendPhoto", chat_id, "photo", file_path, caption);
}

int tg_send_document(tg_client *c, long chat_id, const char *file_path, const char *caption) {
    return tg_upload(c, "sendDocument", chat_id, "document", file_path, caption);
}

int tg_download_file(tg_client *c, const char *file_id, const char *dest_path) {
    if (!c || !file_id || !dest_path) return -1;

    /* Step 1: getFile -> result.file_path */
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "file_id", file_id);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return -1;

    cJSON *root = tg_post(c, "getFile", body);
    free(body);
    if (!root) return -1;

    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON *fp = result ? cJSON_GetObjectItemCaseSensitive(result, "file_path")
                       : NULL;
    if (!cJSON_IsString(fp)) {
        tg_logf("getFile returned no file_path");
        cJSON_Delete(root);
        return -1;
    }
    char *file_path = strdup(fp->valuestring);
    cJSON_Delete(root);
    if (!file_path) return -1;

    /* Step 2: GET https://api.telegram.org/file/bot<token>/<file_path> */
    char url[1024];
    snprintf(url, sizeof url, TG_API_BASE "/file/bot%s/%s", c->token, file_path);
    free(file_path);

    FILE *f = fopen(dest_path, "wb");
    if (!f) {
        tg_logf("cannot open %s for writing", dest_path);
        return -1;
    }

    curl_easy_reset(c->curl);
    curl_easy_setopt(c->curl, CURLOPT_URL, url);
    curl_easy_setopt(c->curl, CURLOPT_WRITEFUNCTION, tg_on_file);
    curl_easy_setopt(c->curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c->curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c->curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c->curl, CURLOPT_XFERINFOFUNCTION, tg_xferinfo);

    CURLcode rc = curl_easy_perform(c->curl);
    long status = 0;
    curl_easy_getinfo(c->curl, CURLINFO_RESPONSE_CODE, &status);
    fclose(f);

    if (rc != CURLE_OK) {
        tg_logf("download: %s", curl_easy_strerror(rc));
        remove(dest_path);
        return -1;
    }
    if (status != 200) {
        tg_logf("download: HTTP %ld", status);
        remove(dest_path);
        return -1;
    }
    return 0;
}

#endif /* TELEGRAM_IMPLEMENTATION */
