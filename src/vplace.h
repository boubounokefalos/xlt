/* XLT - where each video was stopped (cache/videopos.txt), so it carries on
 * from there and the lists can show how far in it is. Portable (host-tested);
 * video.c reads and writes the file. One line per video: ms, tab, length in
 * ms, tab, path. The newest first, at most VP_MAX. */
#ifndef VPLACE_H
#define VPLACE_H

#define VP_MAX 64

typedef struct {
    char path[200];
    int ms, dur;
} VPlace;

typedef struct {
    VPlace p[VP_MAX];
    int n;
} VPlaces;

void vp_parse(VPlaces *v, const char *text, int len);
int vp_write(const VPlaces *v, char *out, int cap); /* length, or -1 if it doesn't fit */

/* Where path was stopped (0 = nowhere); *dur gets its length (may be NULL). */
int vp_get(const VPlaces *v, const char *path, int *dur);
/* Remembers ms of dur for path, first in the list. Barely started (under 15 s)
 * or as good as watched (the last 10%, or the last minute) forgets it instead. */
void vp_set(VPlaces *v, const char *path, int ms, int dur);
void vp_forget(VPlaces *v, const char *path);

#endif
