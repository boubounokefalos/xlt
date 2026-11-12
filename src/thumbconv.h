/* XLT - ICON0 PNG -> 144x80 swizzled 5551 texture. Portable (host-testable). */
#ifndef THUMBCONV_H
#define THUMBCONV_H

#include <stdint.h>

#define THUMB_W 144
#define THUMB_H 80
#define THUMB_BYTES (THUMB_W * THUMB_H * 2)

/* Decodes `png` and writes THUMB_BYTES of swizzled ABGR1555 into `out`.
 * Other sizes are scaled (nearest) to 144x80. Returns 0 on success. */
int thumb_from_png(const void *png, uint32_t size, uint8_t *out);

/* Swizzles 144x80 RGBA8 into the 5551 texture layout. */
void thumb_pack(const uint8_t *rgba, uint8_t *out);

/* A 144x80 centre crop of a JPEG/PNG/BMP picture (not thread-safe: one loader thread). */
int thumb_from_picture(const char *path, uint8_t *out);

/* Music cover (embedded at icon_off, or cover/folder.jpg/.png next to the MP3
 * when icon_off == ICON_FOLDER_ART): an 80x80 square at the left of the 144x80
 * texture, the rest transparent. The UI draws just that square. */
#define THUMB_ART 80
int thumb_from_music(const char *path, uint32_t icon_off, uint8_t *out);

/* A video frame (RGBA8, `stride` pixels a row, fw x fh) cropped to 144:80 at
 * its centre and box-filtered down: a video's thumbnail. Not thread-safe. */
int thumb_from_frame(const uint8_t *rgba, int stride, int fw, int fh, uint8_t *out);

/* Reads ICON0 from a container (disc image or PBP) and converts it. */
int thumb_from_entry(const char *path, int is_disc, uint32_t icon_off, uint32_t icon_size, uint8_t *out);

#endif
