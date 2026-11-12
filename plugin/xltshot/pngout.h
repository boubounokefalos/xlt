/* xltshot - a PNG writer that streams rows out and takes its memory from one
 * block it is given (no malloc: it runs inside a game, from a kernel plugin).
 * Pure C over zlib, so it is tested on the PC (tests/test_pngout.c). */
#ifndef PNGOUT_H
#define PNGOUT_H

#include <stdint.h>
#include <zlib.h>

/* What pngout_begin needs from the caller: zlib's state with a 4 KB window
 * (windowBits 12, memLevel 4: ~30 KB) and the output buffer. */
#define PNGOUT_MEM (48 * 1024)
#define PNGOUT_CHUNK 4096

typedef int (*PngWrite)(void *ctx, const void *data, int len); /* 0 = ok */

typedef struct {
    z_stream z;
    PngWrite write;
    void *ctx;
    uint8_t *arena;
    uint32_t used, cap;
    uint8_t *out;         /* PNGOUT_CHUNK bytes of IDAT data waiting */
    int width, rows_left;
    int err;
} PngOut;

/* Writes the signature and IHDR (8-bit RGB). mem: PNGOUT_MEM bytes. 0 on success. */
int pngout_begin(PngOut *p, void *mem, int width, int height, PngWrite write, void *ctx);
/* One row of width * 3 bytes (R, G, B). */
int pngout_row(PngOut *p, const uint8_t *rgb);
/* The last IDAT and IEND, once every row is in. */
int pngout_end(PngOut *p);

#endif
