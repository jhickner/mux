/* The two chrome rows carried directly above the caret: what is running, and
 * where it is running. Painted as part of the input block, so they survive both
 * a repaint while typing and a repaint under the spinner. */
#ifndef HUD_H
#define HUD_H

/* Both paint from column 0 at `cols`, each row ending in a newline and each
 * exactly one physical row, and return how many they drew. `ud` is the session.
 *
 * The idle rows go above the caret and close with the last turn's summary; the
 * busy ones go above the spinner, which stands in that same row. */
int hud_paint_idle(void *ud, int cols);
int hud_paint_busy(void *ud, int cols);

#endif /* HUD_H */
