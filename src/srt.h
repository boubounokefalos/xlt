/* XLT - SubRip (.srt) subtitles for the video player. Portable (host-tested).
 * The text may be UTF-8, UTF-16 (with its mark), Windows-1252 or Greek
 * Windows-1253 (told apart by how many letters are above ASCII); it is kept
 * as UTF-8 with <i>-style and {\an8}-style tags taken out. */
#ifndef SRT_H
#define SRT_H

typedef struct {
    int start, end;   /* ms */
    const char *text; /* UTF-8, lines split by '\n' */
} SrtCue;

typedef struct {
    SrtCue *cue;
    int n;
    char *buf;        /* the converted text the cues point into */
} Srt;

/* Reads a whole .srt file's bytes. Returns the number of cues (0: none / not one). */
int srt_parse(Srt *s, const char *data, int len);
void srt_free(Srt *s);
/* The cue showing at ms, or NULL. */
const SrtCue *srt_at(const Srt *s, int ms);

#endif
