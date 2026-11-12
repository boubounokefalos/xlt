/* XLT - timed lyrics (LRC): "[01:23.45]a line", the format of .lrc files and
 * of the lyrics many taggers store in an MP3. Portable (host-tested). */
#ifndef LRC_H
#define LRC_H

typedef struct {
    int ms;           /* when the line starts */
    const char *text; /* UTF-8; "" for a pause between lines */
} LrcLine;

typedef struct {
    LrcLine *line;
    int n;
    char *buf;        /* the text the lines point into */
} Lrc;

/* Reads LRC text (UTF-8, UTF-16 with its mark, or an 8-bit code page,
 * guessed as for subtitles). Lines without a time are left out, so lyrics
 * that aren't timed give 0. Several times on one line ("[00:12][01:30]chorus")
 * make one line each; [offset:+ms] is applied; <mm:ss.xx> word times are
 * taken out. Returns the number of lines, in time order. */
int lrc_parse(Lrc *l, const char *data, int len);
void lrc_free(Lrc *l);
/* The line showing at ms (the last one started), -1 before the first. */
int lrc_index(const Lrc *l, int ms);

#endif
