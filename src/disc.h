/* XLT - read files out of ISO, CSO (v1/v2) and ZSO disc images. */
#ifndef DISC_H
#define DISC_H

#include <stdint.h>

enum { DISC_ISO, DISC_CSO, DISC_ZSO };

typedef struct {
    int fd;
    int format;
    uint32_t block_size, align, ver;
    uint64_t total_bytes;
    uint8_t *block;  /* one decompressed block */
    uint8_t *cbuf;   /* compressed input */
    int64_t cached;  /* index of the block in `block`, -1 if none */
    void *z;         /* zlib stream (CSO) */
} Disc;

int disc_open(Disc *d, const char *path); /* 0 on success */
void disc_close(Disc *d);
int disc_read(Disc *d, void *buf, uint32_t size, uint64_t offset); /* 0 on success */

/* Finds "DIR/FILE" (case-insensitive, ";1" ignored). Returns 0 and the byte
 * offset/size of the file in the uncompressed image. */
int disc_find(Disc *d, const char *path, uint32_t *offset, uint32_t *size);

#endif
