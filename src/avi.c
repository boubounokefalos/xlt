/* XLT - MJPEG AVI writer. See avi.h.
 *
 * Layout: RIFF AVI { LIST hdrl { avih, LIST strl { strh, strf } }, LIST movi { 00dc ... }, idx1 }.
 * The header is written first with zero counts and patched by avi_end. */
#include <stdlib.h>
#include <string.h>
#include "avi.h"

#define HDR_SIZE 224          /* up to and including the "movi" fourcc */
#define MOVI_FOURCC_AT 220    /* idx1 offsets count from here */
#define OFF_RIFF_SIZE 4
#define OFF_AVIH_FRAMES 48
#define OFF_AVIH_BUFFER 60
#define OFF_STRH_LENGTH 140
#define OFF_STRH_BUFFER 144
#define OFF_MOVI_SIZE 216

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static int out(Avi *a, const void *p, uint32_t n)
{
    if (a->failed)
        return AVI_ERR_WRITE;
    if (a->write(a->ctx, p, n) < 0) {
        a->failed = 1;
        return AVI_ERR_WRITE;
    }
    a->pos += n;
    return AVI_OK;
}

int avi_begin(Avi *a, int width, int height, int fps, AviWrite write, AviPatch patch, void *ctx)
{
    memset(a, 0, sizeof(*a));
    a->write = write;
    a->patch = patch;
    a->ctx = ctx;
    a->width = width;
    a->height = height;
    a->fps = fps;
    uint8_t h[HDR_SIZE];
    memset(h, 0, sizeof(h));
    memcpy(h, "RIFF", 4);
    memcpy(h + 8, "AVI LIST", 8);
    put32(h + 16, 192);
    memcpy(h + 20, "hdrlavih", 8);
    put32(h + 28, 56);
    uint8_t *v = h + 32;                     /* avih */
    put32(v + 0, 1000000u / (uint32_t)fps);  /* microseconds a frame */
    put32(v + 12, 0x10);                     /* has an index */
    put32(v + 24, 1);                        /* streams */
    put32(v + 32, (uint32_t)width);
    put32(v + 36, (uint32_t)height);
    memcpy(h + 88, "LIST", 4);
    put32(h + 92, 116);
    memcpy(h + 96, "strlstrh", 8);
    put32(h + 104, 56);
    uint8_t *s = h + 108;                    /* strh */
    memcpy(s, "vidsMJPG", 8);
    put32(s + 20, 1);                        /* scale */
    put32(s + 24, (uint32_t)fps);            /* rate: fps frames a second */
    put32(s + 40, 0xFFFFFFFFu);              /* quality: default */
    put16(s + 52, (uint32_t)width);
    put16(s + 54, (uint32_t)height);
    memcpy(h + 164, "strf", 4);
    put32(h + 168, 40);
    uint8_t *b = h + 172;                    /* BITMAPINFOHEADER */
    put32(b + 0, 40);
    put32(b + 4, (uint32_t)width);
    put32(b + 8, (uint32_t)height);
    put16(b + 12, 1);
    put16(b + 14, 24);
    memcpy(b + 16, "MJPG", 4);
    put32(b + 20, (uint32_t)width * (uint32_t)height * 3);
    memcpy(h + 212, "LIST", 4);
    memcpy(h + 220, "movi", 4);
    a->idx_cap = 1024;
    a->idx = malloc(a->idx_cap * 16);
    if (!a->idx)
        return AVI_ERR_NO_MEM;
    return out(a, h, sizeof(h));
}

int avi_frame(Avi *a, const void *jpeg, uint32_t n)
{
    if (a->failed)
        return AVI_ERR_WRITE;
    uint32_t chunk = 8 + n + (n & 1);
    if ((uint64_t)a->pos + chunk + (uint64_t)(a->frames + 1) * 16 + 8 > AVI_MAX_BYTES)
        return AVI_ERR_FULL;
    if (a->frames == a->idx_cap) {
        uint8_t *bigger = realloc(a->idx, (size_t)a->idx_cap * 2 * 16);
        if (!bigger)
            return AVI_ERR_NO_MEM;
        a->idx = bigger;
        a->idx_cap *= 2;
    }
    uint8_t *e = a->idx + (size_t)a->frames * 16;
    memcpy(e, "00dc", 4);
    put32(e + 4, n ? 0x10 : 0); /* every JPEG stands on its own: a key frame */
    put32(e + 8, a->pos - MOVI_FOURCC_AT);
    put32(e + 12, n);
    uint8_t h[8];
    memcpy(h, "00dc", 4);
    put32(h + 4, n);
    int r = out(a, h, 8);
    if (r == AVI_OK && n)
        r = out(a, jpeg, n);
    if (r == AVI_OK && (n & 1))
        r = out(a, "", 1); /* chunks start on even bytes */
    if (r != AVI_OK)
        return r;
    a->frames++;
    if (n) a->pictures++;
    if (n > a->biggest) a->biggest = n;
    return AVI_OK;
}

void avi_abandon(Avi *a)
{
    free(a->idx);
    a->idx = NULL;
}

int avi_end(Avi *a)
{
    uint32_t movi_size = a->pos - (MOVI_FOURCC_AT);
    uint8_t h[8];
    memcpy(h, "idx1", 4);
    put32(h + 4, a->frames * 16);
    int r = out(a, h, 8);
    if (r == AVI_OK && a->frames)
        r = out(a, a->idx, a->frames * 16);
    free(a->idx);
    a->idx = NULL;
    if (r != AVI_OK)
        return r;
    uint8_t v[4];
    struct { uint32_t off, val; } fix[] = {
        {OFF_RIFF_SIZE, a->pos - 8},
        {OFF_AVIH_FRAMES, a->frames},
        {OFF_AVIH_BUFFER, a->biggest + 8},
        {OFF_STRH_LENGTH, a->frames},
        {OFF_STRH_BUFFER, a->biggest + 8},
        {OFF_MOVI_SIZE, movi_size},
    };
    for (unsigned i = 0; i < sizeof(fix) / sizeof(fix[0]); i++) {
        put32(v, fix[i].val);
        if (a->patch(a->ctx, fix[i].off, v, 4) < 0)
            return AVI_ERR_WRITE;
    }
    return AVI_OK;
}
