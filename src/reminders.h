#ifndef REMINDERS_H
#define REMINDERS_H

#include <time.h>
#include <stddef.h>

/*
 * Scheduled reminders, shared between the daemon and the agent. The store is a
 * JSONL file (one reminder per line). A reminder has `text` plus a schedule:
 *
 *   one-shot / fixed interval:
 *     {"at":"2026-07-19 15:00","text":"call the dentist","repeat_secs":0}
 *     (`at` local "YYYY-MM-DD HH:MM"; repeat_secs>0 re-fires that far ahead)
 *
 *   recurrence rule (omit `at`; the daemon computes the next occurrence):
 *     {"text":"...","rule":{"kind":"daily","time":"13:00"}}
 *     {"text":"...","rule":{"kind":"weekdays","time":"13:00"}}       Mon-Fri
 *     {"text":"...","rule":{"kind":"weekly","days":["tue","thu"],"time":"09:00"}}
 *     {"text":"...","rule":{"kind":"nth_weekday","n":2,"dow":"tue","time":"09:00"}}
 *                                                   2nd Tue; n 1-5, or -1 = last
 *     {"text":"...","rule":{"kind":"monthly","dom":15,"time":"09:00"}}
 *
 * The agent creates / lists / cancels reminders by writing this file with its
 * own tools; the daemon fires due ones and reschedules recurring ones.
 */

/* Store path: ~/.config/mux/reminders. */
const char *reminders_path(void);

/*
 * If a reminder is due at `now`, copy its text into `out` (truncated to `n`),
 * remove it from the store (or reschedule it if it repeats), and return 1.
 * Returns 0 when nothing is due. Call repeatedly to drain all due reminders.
 * Re-reads the file each call, so reminders the agent just added are seen.
 */
int reminders_pop_due(time_t now, char *out, size_t n);

#endif /* REMINDERS_H */
