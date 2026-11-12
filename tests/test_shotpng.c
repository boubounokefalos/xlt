/* Host test for PNG screenshots: encode a frame buffer, decode it with libpng, compare. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>
#include "../src/shotpng.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("OK   %s\n", msg); else { printf("FAIL %s\n", msg); fails++; } } while (0)

typedef struct { uint8_t *p; size_t n, cap; int fail_after; } Buf;

static int buf_write(void *ctx, const void *d, int len)
{
    Buf *b = ctx;
    if (b->fail_after >= 0 && b->n + len > (size_t)b->fail_after)
        return -1;
    if (b->n + len > b->cap) {
        b->cap = (b->n + len) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, d, len);
    b->n += len;
    return 0;
}

/* Decodes to RGB; returns 0 and fills w and h on success. */
static int decode(const Buf *b, uint8_t **out, int *w, int *h)
{
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, b->p, b->n))
        return -1;
    img.format = PNG_FORMAT_RGB;
    *out = malloc(PNG_IMAGE_SIZE(img));
    if (!png_image_finish_read(&img, NULL, *out, 0, NULL))
        return -1;
    *w = img.width;
    *h = img.height;
    return 0;
}

static int run(int fmt, int bpp, const char *name)
{
    const int W = 480, H = 272, S = 512;
    uint8_t *fb = calloc((size_t)S * H, bpp);
    uint8_t *want = malloc((size_t)W * H * 3);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            /* full-scale values per channel, so every format maps them exactly */
            int r = (x * 7 + y) % 2 ? 255 : 0, g = x % 3 ? 255 : 0, b = y % 5 ? 0 : 255;
            uint8_t *w = want + ((size_t)y * W + x) * 3;
            w[0] = r; w[1] = g; w[2] = b;
            uint32_t c;
            if (fmt == SHOT_8888) c = r | g << 8 | b << 16 | 0xFF000000u;
            else if (fmt == SHOT_565) c = (r >> 3) | (g >> 2) << 5 | (b >> 3) << 11;
            else if (fmt == SHOT_5551) c = (r >> 3) | (g >> 3) << 5 | (b >> 3) << 10 | 0x8000;
            else c = (r >> 4) | (g >> 4) << 4 | (b >> 4) << 8 | 0xF000;
            if (bpp == 4) ((uint32_t *)fb)[(size_t)y * S + x] = c;
            else ((uint16_t *)fb)[(size_t)y * S + x] = (uint16_t)c;
        }
    Buf b = {0};
    b.fail_after = -1;
    int ok = shot_png(fb, fmt, S, W, H, buf_write, &b) == 0;
    uint8_t *got = NULL;
    int w = 0, h = 0;
    ok = ok && b.n > 8 && !memcmp(b.p, "\x89PNG\r\n\x1a\n", 8);
    ok = ok && decode(&b, &got, &w, &h) == 0 && w == W && h == H && !memcmp(got, want, (size_t)W * H * 3);
    CHECK(ok, name);
    free(got); free(b.p); free(fb); free(want);
    return ok;
}

int main(void)
{
    run(SHOT_565, 2, "565 frame buffer round-trips");
    run(SHOT_5551, 2, "5551 frame buffer round-trips");
    run(SHOT_4444, 2, "4444 frame buffer round-trips");
    run(SHOT_8888, 4, "8888 frame buffer round-trips");

    uint16_t fb[16 * 4] = {0};
    Buf b = {0};
    b.fail_after = 20; /* past the signature, inside IHDR */
    CHECK(shot_png(fb, SHOT_565, 16, 16, 4, buf_write, &b) == -1, "write error is reported");
    free(b.p);
    b = (Buf){0};
    b.fail_after = -1;
    CHECK(shot_png(fb, 7, 16, 16, 4, buf_write, &b) == -1, "unknown format refused");
    CHECK(shot_png(fb, SHOT_565, 8, 16, 4, buf_write, &b) == -1, "stride below width refused");
    free(b.p);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
