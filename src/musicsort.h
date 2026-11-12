/* XLT - the order of the library lists: Music, and Games / Homebrew.
 * Portable (host-tested). */
#ifndef MUSICSORT_H
#define MUSICSORT_H

#include "entry.h"

/* MSORT_ARTIST is the order XLT always had: artist, album, track, title. */
enum { MSORT_ARTIST, MSORT_ALBUM, MSORT_NAME, MSORT_COUNT };

/* "Artist", "Album", "File name" (shown); "artist", "album", "name" (xlt.ini). */
const char *msort_label(int mode);
const char *msort_key(int mode);
int msort_parse(const char *key); /* MSORT_ARTIST if unknown */

/* Songs in the given order. Songs without the tag the order goes by come
 * after all the tagged ones. Never 0 for two different paths. */
int msort_cmp(const Entry *a, const Entry *b, int mode);

/* Text order that ignores case (ASCII) and reads numbers as numbers:
 * "2 Song" before "10 Song". */
int natural_cmp(const char *a, const char *b);

/* Games and Homebrew. "Newest" goes by the file's date (an ISO copied from a
 * PC usually keeps the date it had there, so it's the file's age more than
 * the day it was added). */
enum { GSORT_NAME, GSORT_PLAYED, GSORT_SIZE, GSORT_NEWEST, GSORT_COUNT };

const char *gsort_label(int mode);   /* "Name", "Last played", "Size", "Newest" */
const char *gsort_key(int mode);     /* "name", "played", "size", "newest" (xlt.ini) */
int gsort_parse(const char *key);    /* GSORT_NAME if unknown */
/* played_a / played_b: when each was last started (history.c), 0 = never.
 * Ties, and the never-played ones, go by name. */
int gsort_cmp(const Entry *a, const Entry *b, int mode, uint64_t played_a, uint64_t played_b);

#endif
