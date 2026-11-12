/* xltshot - streaming PNG writer. See pngout.h. */
#include "pngout.h"
#include <string.h>

static void be32(uint8_t *b, uint32_t v) { b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v; }

static int chunk(PngOut *p, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t h[8], c[4];
    be32(h, len);
    memcpy(h + 4, type, 4);
    uLong crc = crc32(0, h + 4, 4);
    if (len) crc = crc32(crc, data, len);
    be32(c, (uint32_t)crc);
    if (p->write(p->ctx, h, 8) || (len && p->write(p->ctx, data, (int)len)) || p->write(p->ctx, c, 4))
        return p->err = -1;
    return 0;
}

/* zlib's memory comes out of the arena, front to back; nothing is given back until the end */
static voidpf take(voidpf opaque, uInt items, uInt size)
{
    PngOut *p = opaque;
    uint32_t n = ((uint32_t)items * size + 15) & ~15u;
    if (p->used + n > p->cap) return Z_NULL;
    voidpf r = p->arena + p->used;
    p->used += n;
    return r;
}
static void give(voidpf opaque, voidpf addr) { (void)opaque; (void)addr; }

int pngout_begin(PngOut *p, void *mem, int width, int height, PngWrite write, void *ctx)
{
    memset(p, 0, sizeof(*p));
    p->write = write;
    p->ctx = ctx;
    p->arena = mem;
    p->cap = PNGOUT_MEM;
    p->width = width;
    p->rows_left = height;
    p->out = take(p, 1, PNGOUT_CHUNK);
    p->z.zalloc = take;
    p->z.zfree = give;
    p->z.opaque = p;
    if (!p->out || deflateInit2(&p->z, 6, Z_DEFLATED, 12, 4, Z_DEFAULT_STRATEGY) != Z_OK)
        return p->err = -1;
    p->z.next_out = p->out;
    p->z.avail_out = PNGOUT_CHUNK;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)width);
    be32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8;  /* bits per sample */
    ihdr[9] = 2;  /* RGB */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    if (write(ctx, sig, 8))
        return p->err = -1;
    return chunk(p, "IHDR", ihdr, 13);
}

/* Feeds zlib; each full output buffer becomes an IDAT chunk. */
static int pump(PngOut *p, const uint8_t *data, uint32_t len, int flush)
{
    p->z.next_in = (Bytef *)data;
    p->z.avail_in = len;
    for (;;) {
        int r = deflate(&p->z, flush);
        if (r == Z_STREAM_ERROR) return p->err = -1;
        if (p->z.avail_out == 0 || (flush == Z_FINISH && r == Z_STREAM_END)) {
            uint32_t n = PNGOUT_CHUNK - p->z.avail_out;
            if (n && chunk(p, "IDAT", p->out, n)) return -1;
            p->z.next_out = p->out;
            p->z.avail_out = PNGOUT_CHUNK;
        }
        if (flush == Z_FINISH ? r == Z_STREAM_END : p->z.avail_in == 0 && p->z.avail_out != 0)
            return 0;
    }
}

int pngout_row(PngOut *p, const uint8_t *rgb)
{
    if (p->err || p->rows_left <= 0) return -1;
    /* filter "Sub" (each byte minus the one a pixel before): screens compress far better */
    static const uint8_t sub = 1;
    if (pump(p, &sub, 1, Z_NO_FLUSH)) return -1;
    uint8_t buf[3 * 64];
    int n = p->width * 3;
    for (int i = 0; i < n; i += (int)sizeof(buf)) {
        int k = n - i < (int)sizeof(buf) ? n - i : (int)sizeof(buf);
        for (int j = 0; j < k; j++)
            buf[j] = (uint8_t)(rgb[i + j] - (i + j >= 3 ? rgb[i + j - 3] : 0));
        if (pump(p, buf, (uint32_t)k, Z_NO_FLUSH)) return -1;
    }
    p->rows_left--;
    return 0;
}

int pngout_end(PngOut *p)
{
    if (p->err || p->rows_left) return -1;
    if (pump(p, NULL, 0, Z_FINISH)) return -1;
    deflateEnd(&p->z);
    return chunk(p, "IEND", NULL, 0);
}
