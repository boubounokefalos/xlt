/* XLT - FLAC decoder and tag reader. Portable (host-tested against the flac
 * encoder: the output has to equal the input exactly). No hardware on the PSP
 * decodes FLAC, so this runs on the main CPU, a block at a time. */
#ifndef FLAC_H
#define FLAC_H

#include <stdint.h>
#include "id3.h"

/* Up to len bytes at off into dst: the count, or < 0. */
typedef int (*FlacReadFn)(void *ctx, uint64_t off, void *dst, uint32_t len);

enum {
    FLAC_ERR_READ = -1,
    FLAC_ERR_FORMAT = -2,
    FLAC_ERR_NO_MEM = -3,
    FLAC_ERR_RATE = -4,  /* a sample rate the PSP can't be given */
    FLAC_ERR_DATA = -5
};

typedef struct {
    FlacReadFn read;
    void *ctx;
    uint64_t file_size, data_start;
    /* reading */
    uint8_t *buf;
    uint32_t pos, len;
    uint64_t buf_off;       /* where buf[0] is in the file */
    uint32_t cache;
    int nbits, overrun;
    /* the stream */
    int rate, channels, bps;
    int min_block, max_block, alloc_block;
    uint64_t total;         /* samples per channel, 0 if unknown */
    int out_rate, step;     /* what the PSP is given: every step'th sample at out_rate */
    /* the last block decoded */
    int32_t *s[8];
    int32_t *mem;
    int block;
    uint64_t frame_sample;  /* its first sample */
    uint32_t skip;          /* samples of it to leave out (after a seek) */
} Flac;

int flac_open(Flac *f, FlacReadFn read, void *ctx, uint64_t file_size);
void flac_close(Flac *f);
/* The next block into f->s: its length, 0 at the end, or FLAC_ERR_*. */
int flac_decode(Flac *f);
/* Goes to a sample; the next block starts there. 0, or FLAC_ERR_*. */
int flac_seek(Flac *f, uint64_t sample);
/* The last block as 16-bit stereo at out_rate: writes up to block*2 shorts, returns frames. */
int flac_output(Flac *f, int16_t *dst);
int flac_duration_ms(const Flac *f);

/* Vorbis comments and the front cover. The cover is only located. 0 if it could be read. */
int flac_tags(const char *path, Id3Info *out);
/* LYRICS or UNSYNCEDLYRICS, as UTF-8. 0 if there are some. */
int flac_lyrics(const char *path, char *out, int cap);

#endif
