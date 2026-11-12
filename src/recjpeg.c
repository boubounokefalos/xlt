/* XLT - screen frames to JPEG with libjpeg. See recjpeg.h.
 * The fast integer DCT and no Huffman optimisation: speed matters more than the
 * last few percent of size, because every millisecond here is a lower frame rate. */
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <jpeglib.h>
#include "recjpeg.h"

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
} Err;

static void on_error(j_common_ptr c)
{
    longjmp(((Err *)c->err)->jump, 1);
}

int rec_jpeg(const uint16_t *fb, int stride, int w, int h, int quality, uint8_t **out, unsigned long *len)
{
    struct jpeg_compress_struct c;
    Err err;
    unsigned char *buf = NULL;
    unsigned long size = 0;
    unsigned char *row = malloc((size_t)w * 3);
    *out = NULL;
    *len = 0;
    if (!row)
        return -1;
    c.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = on_error;
    if (setjmp(err.jump)) {
        jpeg_destroy_compress(&c);
        free(buf);
        free(row);
        return -1;
    }
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &buf, &size);
    c.image_width = (JDIMENSION)w;
    c.image_height = (JDIMENSION)h;
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, quality, TRUE);
    c.dct_method = JDCT_IFAST;
    c.optimize_coding = FALSE;
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        const uint16_t *src = fb + (size_t)c.next_scanline * (size_t)stride;
        for (int x = 0; x < w; x++) {
            uint16_t p = src[x];
            unsigned r = p & 31, g = p >> 5 & 63, b = p >> 11 & 31; /* the GE's 565: red in the low bits */
            row[x * 3] = (unsigned char)(r << 3 | r >> 2);
            row[x * 3 + 1] = (unsigned char)(g << 2 | g >> 4);
            row[x * 3 + 2] = (unsigned char)(b << 3 | b >> 2);
        }
        JSAMPROW rp = row;
        jpeg_write_scanlines(&c, &rp, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    free(row);
    *out = buf;
    *len = size;
    return 0;
}
