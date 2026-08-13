/* Showing a file edit as a diff, by watching the file rather than the tool.
 *
 * The backend is not consulted: whatever performs the edit — the claude CLI,
 * an in-process tool, a shell command — the change is read off disk, so no
 * tool's input schema or result wording is baked in here. */
#ifndef FILEDIFF_H
#define FILEDIFF_H

/* Remember the contents of `path` before a tool runs. Replaces any previous
 * snapshot. A path that does not exist yet snapshots as empty, so creating it
 * reads as all additions. Silently does nothing for a file too large or not
 * readable, in which case filediff_render() draws nothing. */
void filediff_snapshot(const char *path);

/* Diff the snapshot against what is on disk now and print it, indented under
 * the tool row. Returns 1 when something was drawn — an unchanged file draws
 * nothing so the caller can fall back to the tool's own output. Consumes the
 * snapshot either way. */
int filediff_render(void);

/* Render a unified patch supplied directly by a backend. Per-file sections may
 * be introduced by `@@file path` lines; ordinary unified hunk headers and file
 * headers are accepted too. Returns 1 when changed rows were drawn. */
int filediff_render_patch(const char *patch);

/* Drop the snapshot without drawing, for a turn that ended early. */
void filediff_clear(void);

#endif /* FILEDIFF_H */
