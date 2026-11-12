/* XLT - one frame of the screen as a JPEG, for the recorder. Portable (host-tested). */
#ifndef RECJPEG_H
#define RECJPEG_H

#include <stdint.h>

/* fb: 16-bit RGB 565 pixels, `stride` pixels a row. On success *out is a malloc'd
 * JPEG of *len bytes (the caller frees it) and 0 is returned. */
int rec_jpeg(const uint16_t *fb, int stride, int w, int h, int quality, uint8_t **out, unsigned long *len);

#endif
