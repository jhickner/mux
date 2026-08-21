#include "reminders.h"
#include "text.h"
#include "vendor/cJSON.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#define REMINDERS_MAX_BYTES (1u << 24)
#define REMINDERS_PATH_MAX  4300

const char *reminders_path(void)
{
    static char path[4200];
    if (!path[0] && !path_config_file(path, sizeof path, "reminders"))
        snprintf(path, sizeof path, "/tmp/reminders");
    return path;
}

/* "<store><suffix>" into `out`; 0 if it would not fit. */
static int sidecar_path(char *out, size_t n, const char *suffix)
{
    int k = snprintf(out, n, "%s%s", reminders_path(), suffix);
    return k > 0 && (size_t)k < n;
}

/*
 * Advisory lock for the store. The lock lives in a sidecar file rather than the
 * store itself: reminders_pop_due() replaces the store by rename(), so a lock
 * taken on the store's descriptor would end up held on an unlinked inode while
 * the next writer locked the fresh one. The sidecar is never renamed or
 * unlinked, so every participant agrees on one inode.
 *
 * This serializes mux against itself (poller thread vs. main thread, and a
 * second mux process). The agent appends with its own file tools and takes no
 * lock, so the rewrite path additionally re-reads the store under the lock and
 * carries over anything appended behind our back.
 */
static int store_lock(int op)
{
    char lp[REMINDERS_PATH_MAX];
    if (!sidecar_path(lp, sizeof lp, ".lock"))
        return -1;
    int fd = open(lp, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (flock(fd, op) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void store_unlock(int fd)
{
    if (fd >= 0) {
        flock(fd, LOCK_UN);
        close(fd);
    }
}

int reminders_scheduled_count(void)
{
    int   lock = store_lock(LOCK_SH);
    FILE *f = fopen(reminders_path(), "rb");
    if (!f) {
        store_unlock(lock);
        return 0;
    }
    int n = 0, c, content = 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            n += content;
            content = 0;
        } else if (c != ' ' && c != '\t' && c != '\r') {
            content = 1;
        }
    }
    n += content;
    fclose(f);
    store_unlock(lock);
    return n;
}

/* ---- time helpers ----------------------------------------------------- */

/* Parse "YYYY-MM-DD HH:MM[:SS]" (or 'T' separator) as local time; -1 on fail. */
static time_t parse_at(const char *s)
{
    struct tm   tm;
    const char *fmts[] = { "%Y-%m-%d %H:%M:%S", "%Y-%m-%dT%H:%M:%S",
                           "%Y-%m-%d %H:%M",    "%Y-%m-%dT%H:%M", NULL };
    for (int i = 0; fmts[i]; i++) {
        memset(&tm, 0, sizeof tm);
        tm.tm_isdst = -1;
        if (strptime(s, fmts[i], &tm))
            return mktime(&tm);
    }
    return (time_t)-1;
}

static void fmt_at(time_t t, char *out, size_t n)
{
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(out, n, "%Y-%m-%d %H:%M", &lt);
}

static void parse_hhmm(const char *s, int *h, int *m)
{
    *h = 8;                               /* default 08:00 */
    *m = 0;
    if (s)
        sscanf(s, "%d:%d", h, m);
}

/* Day-of-week name/number -> tm_wday (Sun=0..Sat=6), or -1. */
static int dow_num(const char *s)
{
    if (!s)
        return -1;
    static const char *nm[] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
    char b[4] = {0};
    for (int i = 0; i < 3 && s[i]; i++)
        b[i] = (char)tolower((unsigned char)s[i]);
    for (int i = 0; i < 7; i++)
        if (!strcmp(b, nm[i]))
            return i;
    if (s[0] >= '0' && s[0] <= '6' && s[1] == '\0')
        return s[0] - '0';
    return -1;
}

/* ---- recurrence rules ------------------------------------------------- */

/* Does the calendar day of `t` satisfy the rule (ignores time-of-day)? */
static int day_matches(cJSON *rule, const struct tm *t)
{
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(rule, "kind"));
    if (!kind)
        return 0;
    if (!strcmp(kind, "daily"))
        return 1;
    if (!strcmp(kind, "weekdays"))
        return t->tm_wday >= 1 && t->tm_wday <= 5;
    if (!strcmp(kind, "weekly")) {
        cJSON *days = cJSON_GetObjectItem(rule, "days"), *d;
        cJSON_ArrayForEach(d, days)
            if (dow_num(cJSON_GetStringValue(d)) == t->tm_wday)
                return 1;
        return 0;
    }
    if (!strcmp(kind, "nth_weekday")) {
        int    dow = dow_num(cJSON_GetStringValue(cJSON_GetObjectItem(rule, "dow")));
        cJSON *nj = cJSON_GetObjectItem(rule, "n");
        int    nth = nj ? (int)nj->valuedouble : 1;
        if (t->tm_wday != dow)
            return 0;
        if (nth == -1) {                 /* last <dow> of the month */
            struct tm x = *t;
            x.tm_mday += 7;
            x.tm_isdst = -1;
            time_t    tt = mktime(&x);
            struct tm y;
            localtime_r(&tt, &y);
            return y.tm_mon != t->tm_mon;
        }
        return (t->tm_mday - 1) / 7 + 1 == nth;
    }
    if (!strcmp(kind, "monthly")) {
        cJSON *dj = cJSON_GetObjectItem(rule, "dom");
        return t->tm_mday == (dj ? (int)dj->valuedouble : 1);
    }
    return 0;
}

/* Smallest occurrence time >= `after` matching the rule, or -1. Scans forward
 * day by day (handles month/weekday math without edge cases). */
static time_t next_occurrence(cJSON *rule, time_t after)
{
    int hh, mm;
    parse_hhmm(cJSON_GetStringValue(cJSON_GetObjectItem(rule, "time")), &hh, &mm);
    struct tm base;
    localtime_r(&after, &base);
    base.tm_hour = hh;
    base.tm_min = mm;
    base.tm_sec = 0;
    for (int d = 0; d < 420; d++) {      /* ~14 months of lookahead */
        struct tm day = base;
        day.tm_mday = base.tm_mday + d;  /* mktime normalizes overflow + wday */
        day.tm_isdst = -1;
        time_t cand = mktime(&day);
        if (cand < after)
            continue;
        if (day_matches(rule, &day))
            return cand;
    }
    return (time_t)-1;
}

/* ---- store ------------------------------------------------------------ */

/* The effective next-fire time of a reminder: its `at`, or (if absent) the next
 * occurrence of its `rule`, which is written back into the object (*normalized). */
static time_t effective_at(cJSON *o, time_t now, int *normalized)
{
    const char *at = cJSON_GetStringValue(cJSON_GetObjectItem(o, "at"));
    time_t      ts = at ? parse_at(at) : (time_t)-1;
    if (ts != (time_t)-1)
        return ts;
    cJSON *rule = cJSON_GetObjectItem(o, "rule");
    if (rule && cJSON_IsObject(rule)) {
        ts = next_occurrence(rule, now);
        if (ts != (time_t)-1) {
            char b[32];
            fmt_at(ts, b, sizeof b);
            cJSON_DeleteItemFromObject(o, "at");
            cJSON_AddStringToObject(o, "at", b);
            *normalized = 1;
        }
    }
    return ts;
}

/* Set the fired reminder's next `at` (from rule, else repeat_secs), or return 0
 * to drop it (one-shot). */
static int reschedule(cJSON *o, time_t fired_ts, time_t now)
{
    cJSON *rule = cJSON_GetObjectItem(o, "rule");
    if (rule && cJSON_IsObject(rule)) {
        time_t next = next_occurrence(rule, fired_ts + 60);
        if (next == (time_t)-1)
            return 0;
        char b[32];
        fmt_at(next, b, sizeof b);
        cJSON_DeleteItemFromObject(o, "at");
        cJSON_AddStringToObject(o, "at", b);
        return 1;
    }
    cJSON *rep = cJSON_GetObjectItem(o, "repeat_secs");
    long   repeat = (rep && cJSON_IsNumber(rep)) ? (long)rep->valuedouble : 0;
    if (repeat > 0) {
        time_t next = fired_ts + repeat;
        while (next <= now)
            next += repeat;
        char b[32];
        fmt_at(next, b, sizeof b);
        cJSON_DeleteItemFromObject(o, "at");
        cJSON_AddStringToObject(o, "at", b);
        return 1;
    }
    return 0;
}

typedef struct {
    cJSON *o;
    time_t at;
} Ent;

/*
 * Rewrite the store from `ents`. `orig`/`orig_len` are the exact bytes we
 * parsed; the store is re-read first so that lines the agent appended in the
 * meantime survive. If it no longer starts with what we parsed (the agent
 * rewrote or truncated it) the rewrite is skipped rather than clobbering it.
 */
static void store_rewrite(const Ent *ents, int nent, const char *orig, size_t orig_len)
{
    const char *path = reminders_path();
    size_t      cur_len = 0;
    char       *cur = text_slurp(path, REMINDERS_MAX_BYTES, &cur_len);
    if (!cur)
        return;
    if (cur_len < orig_len || memcmp(cur, orig, orig_len) != 0) {
        free(cur);
        return;
    }

    char tmp[REMINDERS_PATH_MAX];
    if (!sidecar_path(tmp, sizeof tmp, ".tmp")) {
        free(cur);
        return;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        free(cur);
        return;
    }
    for (int i = 0; i < nent; i++) {
        if (!ents[i].o)
            continue;
        char *s = cJSON_PrintUnformatted(ents[i].o);
        if (!s) {
            fclose(f);
            unlink(tmp);
            free(cur);
            return;
        }
        fputs(s, f);
        fputc('\n', f);
        free(s);
    }
    if (cur_len > orig_len)
        fwrite(cur + orig_len, 1, cur_len - orig_len, f);
    free(cur);

    int ok = !ferror(f);
    if (fclose(f) != 0)
        ok = 0;
    if (!ok || rename(tmp, path) != 0)
        unlink(tmp);
}

int reminders_pop_due(time_t now, char *out, size_t n)
{
    int lock = store_lock(LOCK_EX);

    size_t len = 0;
    char  *buf = text_slurp(reminders_path(), REMINDERS_MAX_BYTES, &len);
    if (!buf) {
        store_unlock(lock);
        return 0;
    }
    char *orig = malloc(len + 1);
    if (!orig) {
        free(buf);
        store_unlock(lock);
        return 0;
    }
    memcpy(orig, buf, len);
    orig[len] = '\0';

    Ent *ents = NULL;
    int  nobj = 0, cap = 0, dirty = 0, oom = 0;
    for (char *p = buf; *p;) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = '\0';
        if (*p) {
            cJSON *o = cJSON_Parse(p);
            if (o) {
                if (nobj == cap) {
                    int  ncap = cap ? cap * 2 : 64;
                    Ent *ne = realloc(ents, (size_t)ncap * sizeof *ne);
                    if (!ne) {
                        cJSON_Delete(o);
                        oom = 1;
                        break;
                    }
                    ents = ne;
                    cap = ncap;
                }
                ents[nobj].o = o;
                ents[nobj].at = (time_t)-1;
                nobj++;
            }
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    free(buf);

    /* A short parse (allocation failure) must not fire or be written back: the
     * rewrite would drop every line past the failure. */
    if (oom) {
        free(orig);
        for (int i = 0; i < nobj; i++)
            cJSON_Delete(ents[i].o);
        free(ents);
        store_unlock(lock);
        return 0;
    }

    /* Compute each reminder's next-fire time (normalizing rule-only ones). */
    for (int i = 0; i < nobj; i++)
        ents[i].at = effective_at(ents[i].o, now, &dirty);

    /* Fire the earliest that's due. */
    int    fired = -1;
    time_t fired_ts = 0;
    for (int i = 0; i < nobj; i++)
        if (ents[i].at != (time_t)-1 && ents[i].at <= now &&
            (fired < 0 || ents[i].at < fired_ts)) {
            fired = i;
            fired_ts = ents[i].at;
        }

    int result = 0;
    if (fired >= 0) {
        const char *txt = cJSON_GetStringValue(cJSON_GetObjectItem(ents[fired].o, "text"));
        snprintf(out, n, "%s", txt ? txt : "(reminder)");
        if (!reschedule(ents[fired].o, fired_ts, now)) {
            cJSON_Delete(ents[fired].o);
            ents[fired].o = NULL;
        }
        dirty = 1;
        result = 1;
    }

    if (dirty)
        store_rewrite(ents, nobj, orig, len);

    free(orig);
    for (int i = 0; i < nobj; i++)
        if (ents[i].o)
            cJSON_Delete(ents[i].o);
    free(ents);
    store_unlock(lock);
    return result;
}
