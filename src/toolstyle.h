/* How much of a tool call to show. A call that only looks at the workspace —
 * a read, or a shell command built entirely out of read-only pieces — is worth
 * one row and no output preview; anything that acts gets the full block.
 *
 * The judgement is made from the tool's own name and input rather than from a
 * backend's vocabulary, so a driver that names its tools differently still
 * lands in the right tier. */
#ifndef TOOLSTYLE_H
#define TOOLSTYLE_H

/* Nonzero when the call collapses. `input_json` is the tool's input as compact
 * JSON and `arg` a driver's own one-line rendering of it; either may be NULL. */
int toolstyle_collapses(const char *name, const char *input_json, const char *arg);

/* Nonzero when `command` only reads: every stage of it is a known read-only
 * command, nothing redirects or substitutes, and something in it produces
 * output. Anything unrecognized answers 0, so a new command is verbose until
 * it is known to be safe. */
int toolstyle_shell_reads(const char *command);

#endif /* TOOLSTYLE_H */
