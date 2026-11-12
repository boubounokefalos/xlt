/* XLT - writing an MJPEG .avi (AVI 1.0 with an idx1 index), one JPEG per frame
 * at a fixed rate. A frame where nothing changed is an empty chunk, which players
 * show as the frame before, so a still screen costs 8 bytes a frame.
 * Portable (host-tested with ffprobe). */
#ifndef AVI_H
#define AVI_H

#include <stdint.h>

#define AVI_MAX_BYTES (1000u * 1024 * 1024) /* AVI 1.0 players stop trusting a file past 1 GB */

/* All of n bytes appended, or < 0. */
typedef int (*AviWrite)(void *ctx, const void *p, uint32_t n);
/* n bytes written over the file at off (the header, at the end), then back to the end. */
typedef int (*AviPatch)(void *ctx, uint32_t off, const void *p, uint32_t n);

typedef struct {
    AviWrite write;
    AviPatch patch;
    void *ctx;
    int width, height, fps;
    uint32_t frames, pictures; /* frames in all, and how many of them are real pictures */
    uint32_t pos;              /* bytes written */
    uint32_t biggest;
    uint8_t *idx;              /* 16 bytes a frame */
    uint32_t idx_cap;
    int failed;
} Avi;

enum { AVI_OK = 0, AVI_ERR_WRITE = -1, AVI_ERR_FULL = -2, AVI_ERR_NO_MEM = -3 };

int avi_begin(Avi *a, int width, int height, int fps, AviWrite write, AviPatch patch, void *ctx);
/* One frame: a JPEG, or n = 0 for "the same as before". */
int avi_frame(Avi *a, const void *jpeg, uint32_t n);
/* Writes the index and the final sizes, and frees what avi_begin took. */
int avi_end(Avi *a);
/* Frees without finishing (the file is thrown away). */
void avi_abandon(Avi *a);

#endif
