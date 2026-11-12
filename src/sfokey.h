/* XLT - reading one key of a PARAM.SFO (a save's title and details, its
 * folder name). Portable (host-tested). */
#ifndef SFOKEY_H
#define SFOKEY_H

#include <stdint.h>

/* Copies the text (or the number, written out) of `key` into out.
 * Returns 0 if the key is there. */
int sfo_get(const uint8_t *sfo, uint32_t n, const char *key, char *out, int cap);

#endif
