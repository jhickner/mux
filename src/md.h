
#ifndef MD_H
#define MD_H

void md_render(const char *text, int indent);

// md_render, kept in the transcript so a resize re-renders it at the new width
// instead of re-wrapping rows laid out for the old one.
void md_render_kept(const char *text, int indent);

#endif
