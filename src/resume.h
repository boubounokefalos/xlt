/* XLT - the music player's play order (shuffle, repeat) and what it
 * remembers between runs: the list that was
 * playing (cache/music_list.txt) and where in it (cache/music.txt), so
 * playback can carry on after a restart or a game. Portable (host-tested);
 * music.c reads and writes the files.
 *
 * music.txt is a few key=value lines. music_list.txt is one song a line:
 * path, tab, title, tab, artist. Written only when a new list starts. */
#ifndef RESUME_H
#define RESUME_H

enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE, REPEAT_COUNT };

typedef struct {
    int track;        /* index into the list, -1 = nothing to resume (the list ended) */
    int ms;           /* how far into it */
    int shuffle;      /* 0 / 1 */
    int repeat;       /* REPEAT_* */
    char title[128];  /* of that track, to show before the list is read */
    char artist[48];
} ResumeState;

/* Fills st from the text (defaults for anything missing: track -1, off). */
void resume_parse_state(const char *text, ResumeState *st);
/* Returns the length written (or -1 if cap is too small). */
int resume_write_state(char *out, int cap, const ResumeState *st);

/* One song line, appended at out (tabs and newlines in names become spaces).
 * Returns the bytes written, or -1 if it doesn't fit. */
int resume_write_song(char *out, int cap, const char *path, const char *title, const char *artist);

/* Calls fn for each song line of the list text. Returns the count. */
typedef void (*ResumeSongFn)(const char *path, const char *title, const char *artist, void *ctx);
int resume_parse_list(const char *text, int len, ResumeSongFn fn, void *ctx);

/* The play order of n songs: 0..n-1, or shuffled with `first` played first
 * (seed is advanced; any value works). */
void play_order(int *order, int n, int shuffle, int first, unsigned *seed);
/* Where playback goes from position pos in an n-song order: the next one,
 * the start again (repeat all), the same one (repeat one, only when the song
 * ended by itself), or -1 at the end of the list. */
int order_next(int pos, int n, int repeat, int ended);
/* Previous: the one before, the last one from the first with repeat all, else 0. */
int order_prev(int pos, int n, int repeat);

#endif
