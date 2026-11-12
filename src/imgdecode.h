/* XLT - JPEG / PNG / BMP decoding straight to a target size. Portable (host-tested).
 *
 * Rows are read one at a time and sampled into the output, so memory use is a
 * row of the source plus the output (JPEGs are also shrunk by libjpeg while
 * decoding: 1/2, 1/4 or 1/8). A 12-megapixel photo never needs a full-size
 * buffer, which the PSP would not have. */
#ifndef IMGDECODE_H
#define IMGDECODE_H

#include <stdint.h>

enum {
    IMG_COVER, /* fill ow x oh exactly, cropping the centre (thumbnails) */
    IMG_FIT,   /* whole picture, aspect kept, at most ow x oh (the viewer) */
};

/* Decodes the file at path into out (RGBA8, row stride ow * 4).
 * *rw / *rh receive the size actually written (= ow/oh for IMG_COVER).
 * Returns 0 on success. Pictures larger than 8192 px on a side are refused. */
int img_decode(const char *path, int mode, int ow, int oh, uint8_t *out, int *rw, int *rh);

/* Same, for an image embedded in a bigger file (an MP3's cover art):
 * the image data starts at byte `offset`. JPEG and PNG only. */
int img_decode_at(const char *path, uint32_t offset, int mode, int ow, int oh, uint8_t *out, int *rw, int *rh);

/* Same, for an image in memory (a web page's picture): JPEG, PNG or GIF (its first frame). */
int img_decode_mem(const uint8_t *buf, int len, int mode, int ow, int oh, uint8_t *out, int *rw, int *rh);

/* 1 if the name has a picture extension (.jpg .jpeg .png .bmp). */
int img_is_picture(const char *name);

/* "JPEG", "PNG", "BMP" from the extension, "Picture" otherwise. */
const char *img_format_name(const char *path);

#endif
