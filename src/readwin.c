#include <string.h>
#include "readwin.h"

void readwin_init(ReadWin *w, uint8_t *mem, uint32_t cap, ReadWinFn read, void *ctx)
{
    w->mem = mem;
    w->cap = mem ? cap : 0;
    w->have = 0;
    w->start = 0;
    w->read = read;
    w->ctx = ctx;
}

int readwin_get(ReadWin *w, uint64_t off, uint32_t len, void *dst)
{
    if (len > w->cap) /* no window, or bigger than it: the window stays as it is */
        return w->read(w->ctx, off, dst, len) == (int)len ? 0 : -1;
    if (off < w->start || off + len > w->start + w->have) {
        int got = w->read(w->ctx, off, w->mem, w->cap);
        if (got < (int)len) { /* short near the end of the file is fine if it covers this */
            w->have = 0;
            return -1;
        }
        w->start = off;
        w->have = (uint32_t)got;
    }
    memcpy(dst, w->mem + (uint32_t)(off - w->start), len);
    return 0;
}
