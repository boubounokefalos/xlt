/* XLT - encodes a frame buffer as PNG. Portable (host-tested). */
#ifndef SHOTPNG_H
#define SHOTPNG_H

#include <stdint.h>

/* Pixel formats, same values as PSP_DISPLAY_PIXEL_FORMAT_*. */
enum { SHOT_565 = 0, SHOT_5551 = 1, SHOT_4444 = 2, SHOT_8888 = 3 };

/* Writes all len bytes; returns 0 on success. */
typedef int (*ShotWrite)(void *ctx, const void *data, int len);

/* Encodes w x h pixels of fb (stride pixels per row, R in the low bits as on
 * the PSP) as a 24-bit RGB PNG through write. Returns 0 on success. */
int shot_png(const void *fb, int fmt, int stride, int w, int h, ShotWrite write, void *ctx);

#endif
