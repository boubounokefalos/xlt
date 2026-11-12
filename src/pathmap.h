/* XLT - a small remembered number per file (a picture's rotation), kept as
 * text: one "value<TAB>path" line each, the newest first, at most PM_MAX.
 * Portable (host-tested); the owner reads and writes the file. */
#ifndef PATHMAP_H
#define PATHMAP_H

#define PM_MAX 256

typedef struct {
    struct { char path[200]; int value; } e[PM_MAX];
    int n;
} PathMap;

void pm_parse(PathMap *m, const char *text, int len);
int pm_write(const PathMap *m, char *out, int cap); /* length, or -1 if it doesn't fit */
int pm_get(const PathMap *m, const char *path);    /* 0 if it has none */
void pm_set(PathMap *m, const char *path, int value); /* first in the list; 0 forgets it */

#endif
