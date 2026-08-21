#include "reminders.h"
#include "text.h"
#include "vendor/cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REMINDERS_MAX_BYTES (1u << 20)

const char *reminders_path(void)
{
    static char path[4200];
    if (!path[0] && !path_config_file(path, sizeof path, "reminders"))
        snprintf(path, sizeof path, "/tmp/reminders");
    return path;
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

int reminders_pop_due(time_t now, char *out, size_t n)
{
    char *buf = text_slurp(reminders_path(), REMINDERS_MAX_BYTES, NULL);
    if (!buf)
        return 0;

    cJSON *objs[512];
    time_t ats[512];
    int    nobj = 0, dirty = 0;
    for (char *p = buf; *p && nobj < 512;) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = '\0';
        if (*p) {
            cJSON *o = cJSON_Parse(p);
            if (o)
                objs[nobj++] = o;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    free(buf);

    /* Compute each reminder's next-fire time (normalizing rule-only ones). */
    for (int i = 0; i < nobj; i++)
        ats[i] = effective_at(objs[i], now, &dirty);

    /* Fire the earliest that's due. */
    int    fired = -1;
    time_t fired_ts = 0;
    for (int i = 0; i < nobj; i++)
        if (ats[i] != (time_t)-1 && ats[i] <= now && (fired < 0 || ats[i] < fired_ts)) {
            fired = i;
            fired_ts = ats[i];
        }

    int result = 0;
    if (fired >= 0) {
        const char *txt = cJSON_GetStringValue(cJSON_GetObjectItem(objs[fired], "text"));
        snprintf(out, n, "%s", txt ? txt : "(reminder)");
        if (!reschedule(objs[fired], fired_ts, now)) {
            cJSON_Delete(objs[fired]);
            objs[fired] = NULL;
        }
        dirty = 1;
        result = 1;
    }

    if (dirty) {
        const char *path = reminders_path();
        char        tmp[640];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        if (f) {
            for (int i = 0; i < nobj; i++) {
                if (!objs[i])
                    continue;
                char *s = cJSON_PrintUnformatted(objs[i]);
                if (s) {
                    fputs(s, f);
                    fputc('\n', f);
                    free(s);
                }
            }
            fclose(f);
            rename(tmp, path);
        }
    }
    for (int i = 0; i < nobj; i++)
        if (objs[i])
            cJSON_Delete(objs[i]);
    return result;
}
