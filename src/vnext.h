/* XLT - which video plays after this one (Up next). */
#ifndef VNEXT_H
#define VNEXT_H

/* 1 if the name ends in a video extension the player takes (.mp4, .m4v, .pmf). */
int vnext_is_video(const char *name);
/* Feed every file name in the folder; *best keeps the next video after me:
 * the smallest name above it in natural order (Episode 2 before Episode 10). */
void vnext_consider(const char *me, const char *name, char *best, int n);

#endif
