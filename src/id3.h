/* XLT - MP3 tag reading: ID3v2.2/2.3/2.4 and ID3v1. Portable (host-tested).
 * Frames are read one at a time from the file; cover art is only located
 * (offset and size), never loaded, so a 1 MB embedded picture costs nothing. */
#ifndef ID3_H
#define ID3_H

#include <stdint.h>

typedef struct {
    char title[128];  /* UTF-8; "" if the file has none */
    char artist[48];
    char album[48];
    int track;        /* 0 if unknown */
    uint32_t art_off; /* JPEG/PNG data of the front cover (or the first picture), 0 if none */
    uint32_t art_size;
    uint32_t audio_start; /* first byte after the ID3v2 tag */
    /* loudness tags, in hundredths of a dB (what to add to play at the reference level) */
    int gains;            /* which of these the file has: GAIN_* bits */
    int gain_track, gain_album; /* ReplayGain */
    int gain_sc;          /* iTunes Sound Check (iTunNORM) */
} Id3Info;

enum { GAIN_TRACK = 1, GAIN_ALBUM = 2, GAIN_SC = 4 };

/* A tag the readers met: key "REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_ALBUM_GAIN" (any
 * case; value like "-7.23 dB") or "iTunNORM" (Sound Check's ten hex numbers). Others
 * are ignored. */
void id3_take_gain(Id3Info *o, const char *key, const char *value);

/* file_size: as the scanner got it from stat (for ID3v1 at the end).
 * Returns 0 if the file could be read (tags may still be empty). */
int id3_read(const char *path, uint64_t file_size, Id3Info *out);

/* The lyrics stored in the tag (USLT, or ULT in v2.2), as UTF-8 in out (lines
 * split by '\n', cut to cap). Returns 0 if there are some. Players often put
 * timed LRC text here; lrc.c reads either. */
int id3_lyrics(const char *path, char *out, int cap);

#endif
