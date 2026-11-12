/* XLT - Music: the Artists and Albums lists, made from the songs' tags.
 * Portable (host-tested): it only sees the library through a getter.
 *
 * Artists go by the artist tag (case ignored); songs without one are under
 * "Unknown artist", last. Albums go by the album tag *and* the song's folder,
 * so two "Greatest Hits" of different artists stay apart while a compilation
 * (one folder, many artists) stays together; songs without an album tag are
 * grouped by their folder, named after it (loose ones in MUSIC/ itself have
 * no name: "Unknown album", last). An empty name means unknown. */
#ifndef MBROWSE_H
#define MBROWSE_H

#include "entry.h"

enum { MB_ARTISTS, MB_ALBUMS, MB_KINDS };

#define MB_NAME 48

typedef struct {
    char name[MB_NAME];
    char sub[MB_NAME]; /* albums: the artist, or "Various artists"; artists: "" */
    int count;         /* songs */
    int albums;        /* artists: how many albums */
    int cover;         /* library index of a song to show the cover of */
} MbGroup;

/* Rebuilds both lists from n songs (get(i) for i < n, the library's order).
 * 0, or -1 if out of memory (the lists are then empty). */
int mb_build(const Entry *(*get)(int i), int n);
void mb_free(void);

int mb_count(int kind);
const MbGroup *mb_group(int kind, int k);
/* The group of library song i, or -1. */
int mb_group_of(int kind, int i);

/* The songs of group k as library indices, in play order (artists: album,
 * track, title; albums: track, title). Returns how many (at most max). */
int mb_songs(int kind, int k, int *out, int max);

#endif
