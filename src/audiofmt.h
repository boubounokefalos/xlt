/* XLT - which music files XLT plays, and their tags whatever the format.
 * Portable (host-tested). */
#ifndef AUDIOFMT_H
#define AUDIOFMT_H

#include <stdint.h>
#include "id3.h"

enum { AUDIO_NONE, AUDIO_MP3, AUDIO_AAC, AUDIO_M4A, AUDIO_FLAC };

/* By the file's name. AUDIO_NONE if it isn't music. */
int audio_format(const char *name);
/* For the Files panel: "MP3 audio", "FLAC audio"... */
const char *audio_format_name(int format);

int audio_tags(const char *path, uint64_t file_size, Id3Info *out);
int audio_lyrics(const char *path, char *out, int cap);

/* Volume levelling (Settings: g_cfg.level). LEVEL_SONG uses each song's own
 * loudness tag, LEVEL_ALBUM the album's (so an album's quiet and loud songs
 * stay apart), each falling back on the other and then on Sound Check. */
enum { LEVEL_OFF, LEVEL_SONG, LEVEL_ALBUM, LEVEL_COUNT };
#define GAIN_NONE (-100000)
/* The gain to use from t's tags, in hundredths of a dB; GAIN_NONE if it has none. */
int audio_gain(const Id3Info *t, int mode);
/* The output volume (of max) for that gain. It only ever turns a song down,
 * never up: the tags' reference level is about 6 dB quieter than most music,
 * so LEVEL_HEADROOM is added first and anything above 0 dB plays as it is.
 * Nothing gets louder than without levelling, and the PSP's own volume and
 * AVLS limit still apply after it. */
#define LEVEL_HEADROOM 600
int audio_level_volume(int gain, int max);

/* 1 if the PSP's audio output can be given this sample rate. */
int audio_rate_ok(int rate);

/* An ADTS (.aac) frame header. */
typedef struct {
    int object;   /* 2 = AAC LC */
    int rate, channels;
    int header;   /* its bytes (7 or 9) */
    int frame;    /* bytes of the whole frame, header included */
} Adts;

/* The header at p (n bytes readable). 0 if valid. */
int adts_parse(const uint8_t *p, int n, Adts *out);
/* The first offset in p where a frame starts and, if it fits, another follows. -1 if none. */
int adts_find(const uint8_t *p, int n);

#endif
