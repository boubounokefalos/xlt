/* XLT - ICON0 thumbnails: disk cache + VRAM LRU, loaded on a second thread. */
#ifndef THUMBS_H
#define THUMBS_H

#include <stdint.h>
#include "entry.h"

void thumbs_init(const char *app_dir);
void thumbs_shutdown(void);

/* Main thread, before drawing: uploads finished thumbnails to VRAM.
 * Returns 1 if any arrived (so the screen should redraw). */
int thumbs_pump(void);

/* Returns the VRAM texture for e, or NULL (and queues a load) if not resident. */
const void *thumbs_get(const Entry *e);

/* 1 if e has no thumbnail (its load failed, or a video's frame isn't cached yet). */
int thumbs_missing(const Entry *e);
/* Main thread, between frames: a thumbnail made elsewhere (a video's frame);
 * shown at once and written to the cache on the background worker. */
void thumbs_put(uint32_t key, const uint8_t *tex);

#endif
