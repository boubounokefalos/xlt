/* Reads a file through one window of memory, so playing a video costs one
 * Memory Stick read every second or so instead of a seek and read per frame. */
#ifndef READWIN_H
#define READWIN_H

#include <stdint.h>

/* Reads up to len bytes at off into dst; returns the number read, or < 0. */
typedef int (*ReadWinFn)(void *ctx, uint64_t off, void *dst, uint32_t len);

typedef struct {
    uint8_t *mem;       /* NULL / cap 0: every read goes straight to the file */
    uint32_t cap, have;
    uint64_t start;     /* file offset of mem[0] */
    ReadWinFn read;
    void *ctx;
} ReadWin;

void readwin_init(ReadWin *w, uint8_t *mem, uint32_t cap, ReadWinFn read, void *ctx);

/* Copies exactly len bytes at off to dst. Returns 0, or -1 if they can't be read. */
int readwin_get(ReadWin *w, uint64_t off, uint32_t len, void *dst);

#endif
