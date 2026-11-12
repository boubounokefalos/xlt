/* XLT - PNG screenshots with libpng. See shotpng.h.
 * Fast settings (zlib level 1, Sub filter): flat UI colours still compress
 * well, and the save takes a fraction of a second on the PSP. */
#include <setjmp.h>
#include <stdlib.h>
#include <png.h>
#include "shotpng.h"

typedef struct {
    ShotWrite write;
    void *ctx;
} Sink;

static void sink_write(png_structp p, png_bytep data, png_size_t len)
{
    Sink *s = png_get_io_ptr(p);
    if (s->write(s->ctx, data, (int)len) != 0)
        png_error(p, "write");
}

static void sink_flush(png_structp p) { (void)p; } /* libpng's default would fflush() a FILE* */
static void quiet_warn(png_structp p, png_const_charp m) { (void)p; (void)m; }
static void quiet_err(png_structp p, png_const_charp m) { (void)m; png_longjmp(p, 1); } /* no stderr print */

static void to_rgb(const void *fb, int fmt, int stride, int y, int w, uint8_t *out)
{
    if (fmt == SHOT_8888) {
        const uint32_t *src = (const uint32_t *)fb + (size_t)y * stride;
        for (int x = 0; x < w; x++, out += 3) {
            uint32_t c = src[x];
            out[0] = c & 0xFF;
            out[1] = (c >> 8) & 0xFF;
            out[2] = (c >> 16) & 0xFF;
        }
        return;
    }
    const uint16_t *src = (const uint16_t *)fb + (size_t)y * stride;
    for (int x = 0; x < w; x++, out += 3) {
        uint16_t c = src[x];
        unsigned r, g, b;
        if (fmt == SHOT_565) {
            r = c & 0x1F; g = (c >> 5) & 0x3F; b = c >> 11;
            out[0] = (r << 3) | (r >> 2);
            out[1] = (g << 2) | (g >> 4);
            out[2] = (b << 3) | (b >> 2);
        } else if (fmt == SHOT_5551) {
            r = c & 0x1F; g = (c >> 5) & 0x1F; b = (c >> 10) & 0x1F;
            out[0] = (r << 3) | (r >> 2);
            out[1] = (g << 3) | (g >> 2);
            out[2] = (b << 3) | (b >> 2);
        } else { /* 4444 */
            r = c & 0xF; g = (c >> 4) & 0xF; b = (c >> 8) & 0xF;
            out[0] = r * 17;
            out[1] = g * 17;
            out[2] = b * 17;
        }
    }
}

int shot_png(const void *fb, int fmt, int stride, int w, int h, ShotWrite write, void *ctx)
{
    if (fmt < SHOT_565 || fmt > SHOT_8888 || w <= 0 || h <= 0 || stride < w)
        return -1;
    uint8_t *volatile row = malloc((size_t)w * 3);
    if (!row)
        return -1;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, quiet_err, quiet_warn);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!info || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, info ? &info : NULL);
        free(row);
        return -1;
    }
    Sink sink = {write, ctx};
    png_set_write_fn(png, &sink, sink_write, sink_flush);
    png_set_compression_level(png, 1);
    png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_SUB);
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (int y = 0; y < h; y++) {
        to_rgb(fb, fmt, stride, y, w, row);
        png_write_row(png, row);
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    free(row);
    return 0;
}
