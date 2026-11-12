/* XLT - title matching for search. Portable (host-tested). */
#ifndef MATCH_H
#define MATCH_H

/* Score of `title` for `query` (case-insensitive), 0 = no match.
 * Every space-separated query word must occur in the title. Higher is
 * better: whole title starts with the query > every word starts a title
 * word > plain substrings. */
int match_score(const char *title, const char *query);

#endif
