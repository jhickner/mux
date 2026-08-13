/* Markdown rendering for assistant replies: block structure (headings, lists,
 * fenced code, quotes, rules) plus inline spans, wrapped to the terminal. */
#ifndef MD_H
#define MD_H

/* Render `text` starting at column `indent`, consuming its trailing newline. */
void md_render(const char *text, int indent);

#endif /* MD_H */
