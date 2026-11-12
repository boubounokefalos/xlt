/* XLT - ICON0 decoding. Runs once per game; the result is cached on the
 * Memory Stick so the PNG never has to be decoded again. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <png.h>
#include "thumbconv.h"
#include "disc.h"
#include "xio.h"
#include "imgdecode.h"

#define ICON_MAX (512 * 1024)

void thumb_pack(const uint8_t *rgba, uint8_t *out)
{
    /* Convert + swizzle in one pass. Swizzled layout: 16-byte x 8-row blocks
     * in raster order; a row of 144 px (288 bytes) is 18 blocks of 8 px. */
    const int bw = THUMB_W * 2, blocks_x = bw / 16;
    for (int y = 0; y < THUMB_H; y++) {
        const uint8_t *row = rgba + (size_t)y * THUMB_W * 4;
        for (int x = 0; x < THUMB_W; x++) {
            const uint8_t *p = row + (size_t)x * 4;
            uint16_t v = (p[3] >= 128 ? 0x8000 : 0) | (p[2] >> 3) << 10 | (p[1] >> 3) << 5 | (p[0] >> 3);
            int bx = (x * 2) / 16, in = (x * 2) % 16;
            size_t o = ((size_t)(y / 8) * blocks_x + bx) * 128 + (y % 8) * 16 + in;
            out[o] = v & 0xFF;
            out[o + 1] = v >> 8;
        }
    }
}

int thumb_from_picture(const char *path, uint8_t *out)
{
    static uint8_t rgba[THUMB_W * THUMB_H * 4];
    int w, h;
    if (img_decode(path, IMG_COVER, THUMB_W, THUMB_H, rgba, &w, &h) != 0)
        return -1;
    thumb_pack(rgba, out);
    return 0;
}

int thumb_from_music(const char *path, uint32_t icon_off, uint8_t *out)
{
    static uint8_t art[THUMB_ART * THUMB_ART * 4], rgba[THUMB_W * THUMB_H * 4];
    int w, h, r = -1;
    if (icon_off == 0xFFFFFFFFu) { /* ICON_FOLDER_ART */
        static const char *names[] = {"cover.jpg", "folder.jpg", "cover.png", "folder.png"};
        const char *slash = strrchr(path, '/');
        int n = slash ? (int)(slash - path) : 0;
        char p[300];
        for (int i = 0; i < 4 && r != 0; i++) {
            snprintf(p, sizeof(p), "%.*s/%s", n, path, names[i]);
            r = img_decode(p, IMG_COVER, THUMB_ART, THUMB_ART, art, &w, &h);
        }
    } else {
        r = img_decode_at(path, icon_off, IMG_COVER, THUMB_ART, THUMB_ART, art, &w, &h);
    }
    if (r != 0)
        return -1;
    memset(rgba, 0, sizeof(rgba));
    for (int y = 0; y < THUMB_ART; y++)
        memcpy(rgba + (size_t)y * THUMB_W * 4, art + (size_t)y * THUMB_ART * 4, THUMB_ART * 4);
    thumb_pack(rgba, out);
    return 0;
}

int thumb_from_png(const void *png, uint32_t size, uint8_t *out)
{
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, png, size))
        return -1;
    img.format = PNG_FORMAT_RGBA;
    if (img.width == 0 || img.height == 0 || img.width > 1024 || img.height > 1024) {
        png_image_free(&img);
        return -1;
    }
    uint8_t *rgba = malloc(PNG_IMAGE_SIZE(img));
    if (!rgba) {
        png_image_free(&img);
        return -1;
    }
    if (!png_image_finish_read(&img, NULL, rgba, 0, NULL)) {
        free(rgba);
        png_image_free(&img);
        return -1;
    }

    if (img.width == THUMB_W && img.height == THUMB_H) {
        thumb_pack(rgba, out);
    } else {
        /* off-size ICON0: nearest-scale to 144x80 first */
        static uint8_t scaled[THUMB_W * THUMB_H * 4];
        for (int y = 0; y < THUMB_H; y++)
            for (int x = 0; x < THUMB_W; x++)
                memcpy(scaled + ((size_t)y * THUMB_W + x) * 4,
                       rgba + ((size_t)(y * (int)img.height / THUMB_H) * img.width + x * (int)img.width / THUMB_W) * 4, 4);
        thumb_pack(scaled, out);
    }
    free(rgba);
    png_image_free(&img);
    return 0;
}

int thumb_from_entry(const char *path, int is_disc, uint32_t off, uint32_t size, uint8_t *out)
{
    if (size == 0 || size > ICON_MAX)
        return -1;
    uint8_t *png = malloc(size);
    if (!png)
        return -1;
    int r = -1;
    if (is_disc) {
        Disc d;
        if (!disc_open(&d, path)) {
            r = disc_read(&d, png, size, off);
            disc_close(&d);
        }
    } else {
        int fd = xio_open(path);
        if (fd >= 0) {
            r = xio_read_at(fd, png, size, off) == (int)size ? 0 : -1;
            xio_close(fd);
        }
    }
    if (!r)
        r = thumb_from_png(png, size, out);
    free(png);
    return r;
}

int thumb_from_frame(const uint8_t *rgba, int stride, int fw, int fh, uint8_t *out)
{
    if (!rgba || fw <= 0 || fh <= 0 || stride < fw)
        return -1;
    static uint8_t tmp[THUMB_W * THUMB_H * 4];
    /* centre crop to 144:80, then a box filter over each output pixel's area */
    int cw = fw, ch = fh;
    if ((long)fw * THUMB_H > (long)fh * THUMB_W) cw = fh * THUMB_W / THUMB_H;
    else ch = fw * THUMB_H / THUMB_W;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    int x0 = (fw - cw) / 2, y0 = (fh - ch) / 2;
    for (int y = 0; y < THUMB_H; y++) {
        int sy0 = y0 + y * ch / THUMB_H, sy1 = y0 + (y + 1) * ch / THUMB_H;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < THUMB_W; x++) {
            int sx0 = x0 + x * cw / THUMB_W, sx1 = x0 + (x + 1) * cw / THUMB_W;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint8_t *p = rgba + ((size_t)sy * stride + sx0) * 4;
                for (int sx = sx0; sx < sx1; sx++, p += 4) {
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    n++;
                }
            }
            uint8_t *o = tmp + ((size_t)y * THUMB_W + x) * 4;
            o[0] = (uint8_t)(r / n);
            o[1] = (uint8_t)(g / n);
            o[2] = (uint8_t)(b / n);
            o[3] = 255; /* video frames are opaque */
        }
    }
    thumb_pack(tmp, out);
    return 0;
}
