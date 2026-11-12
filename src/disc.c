/* XLT - ISO9660 / CSO / ZSO reader.
 *
 * CSO/ZSO layout: 24-byte header ("CISO"/"ZISO", header size, total bytes,
 * block size, version, index shift), then a u32 index per block (+1). The
 * top bit of an index entry is a flag, the rest is the file offset >> align.
 *   CSO v1: flag = stored, otherwise raw deflate
 *   CSO v2: size >= block size = stored, flag = LZ4, otherwise raw deflate
 *   ZSO:    flag = stored, otherwise LZ4
 * Index entries are read on demand, so opening an image costs one read.
 */
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <lz4.h>
#include "disc.h"
#include "xio.h"

#define SECTOR 2048
#define MAX_BLOCK (64 * 1024)

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int disc_open(Disc *d, const char *path)
{
    memset(d, 0, sizeof(*d));
    d->cached = -1;
    d->fd = xio_open(path);
    if (d->fd < 0)
        return -1;

    uint8_t h[24];
    if (xio_read_at(d->fd, h, 24, 0) != 24)
        goto fail;
    if (!memcmp(h, "CISO", 4) || !memcmp(h, "ZISO", 4)) {
        d->format = h[0] == 'C' ? DISC_CSO : DISC_ZSO;
        d->total_bytes = rd32(h + 8) | (uint64_t)rd32(h + 12) << 32;
        d->block_size = rd32(h + 16);
        d->ver = h[20];
        d->align = h[21];
        if (d->block_size < SECTOR || d->block_size > MAX_BLOCK || (d->block_size & (SECTOR - 1)))
            goto fail;
        d->block = malloc(d->block_size);
        d->cbuf = malloc(d->block_size + 64);
        if (!d->block || !d->cbuf)
            goto fail;
        if (d->format == DISC_CSO) {
            z_stream *z = calloc(1, sizeof(z_stream));
            if (!z || inflateInit2(z, -15) != Z_OK) {
                free(z);
                goto fail;
            }
            d->z = z;
        }
    } else {
        d->format = DISC_ISO;
        d->block_size = SECTOR;
    }
    return 0;
fail:
    disc_close(d);
    return -1;
}

void disc_close(Disc *d)
{
    if (d->z) {
        inflateEnd(d->z);
        free(d->z);
    }
    free(d->block);
    free(d->cbuf);
    if (d->fd >= 0)
        xio_close(d->fd);
    memset(d, 0, sizeof(*d));
    d->fd = -1;
}

static int inflate_block(Disc *d, uint32_t csize)
{
    z_stream *z = d->z;
    inflateReset(z);
    z->next_in = d->cbuf;
    z->avail_in = csize;
    z->next_out = d->block;
    z->avail_out = d->block_size;
    int r = inflate(z, Z_FINISH);
    return (r == Z_STREAM_END || z->avail_out == 0) ? 0 : -1;
}

static int load_block(Disc *d, uint32_t n)
{
    if ((int64_t)n == d->cached)
        return 0;
    uint8_t ix[8];
    if (xio_read_at(d->fd, ix, 8, 24 + (uint64_t)n * 4) != 8)
        return -1;
    uint32_t i0 = rd32(ix), i1 = rd32(ix + 4);
    int flag = i0 >> 31;
    uint64_t pos = (uint64_t)(i0 & 0x7FFFFFFF) << d->align;
    uint64_t end = (uint64_t)(i1 & 0x7FFFFFFF) << d->align;
    if (end < pos)
        return -1;
    uint32_t csize = (uint32_t)(end - pos);

    int stored = d->format == DISC_CSO && d->ver >= 2 ? csize >= d->block_size : flag;
    int lz4 = d->format == DISC_ZSO || (d->ver >= 2 && flag);
    d->cached = -1;
    if (stored) {
        if (xio_read_at(d->fd, d->block, d->block_size, pos) <= 0)
            return -1; /* the last block may be short; the rest stays as is */
    } else {
        if (csize > d->block_size + 64)
            csize = d->block_size + 64;
        if (xio_read_at(d->fd, d->cbuf, csize, pos) <= 0)
            return -1;
        if (lz4) {
            int r = LZ4_decompress_safe_partial((const char *)d->cbuf, (char *)d->block,
                                                (int)csize, (int)d->block_size, (int)d->block_size);
            if (r <= 0)
                return -1;
        } else if (!d->z || inflate_block(d, csize)) {
            return -1;
        }
    }
    d->cached = n;
    return 0;
}

int disc_read(Disc *d, void *buf, uint32_t size, uint64_t off)
{
    if (d->format == DISC_ISO)
        return xio_read_at(d->fd, buf, size, off) == (int)size ? 0 : -1;
    if (off + size > d->total_bytes)
        return -1;
    uint8_t *out = buf;
    while (size) {
        uint32_t n = (uint32_t)(off / d->block_size), in = (uint32_t)(off % d->block_size);
        uint32_t take = d->block_size - in;
        if (take > size)
            take = size;
        if (load_block(d, n))
            return -1;
        memcpy(out, d->block + in, take);
        out += take;
        off += take;
        size -= take;
    }
    return 0;
}

/* Compares an ISO9660 name ("PARAM.SFO;1") to a plain one, ignoring case. */
static int name_eq(const uint8_t *iso, int len, const char *name, int nlen)
{
    for (int i = 0; i < len; i++)
        if (iso[i] == ';') {
            len = i;
            break;
        }
    if (len > 0 && iso[len - 1] == '.')
        len--;
    if (len != nlen)
        return 0;
    for (int i = 0; i < len; i++) {
        int a = iso[i], b = (unsigned char)name[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b)
            return 0;
    }
    return 1;
}

/* Looks for `name` in the directory at (lba, size). */
static int find_in_dir(Disc *d, uint32_t lba, uint32_t size, const char *name, int nlen,
                       uint32_t *out_lba, uint32_t *out_size, int *is_dir)
{
    uint8_t sec[SECTOR];
    if (size > 64 * SECTOR)
        size = 64 * SECTOR;
    for (uint32_t s = 0; s * SECTOR < size; s++) {
        if (disc_read(d, sec, SECTOR, (uint64_t)(lba + s) * SECTOR))
            return -1;
        uint32_t p = 0;
        while (p + 33 < SECTOR) {
            uint8_t len = sec[p];
            if (len == 0)
                break; /* rest of this sector is padding */
            if (p + len > SECTOR || len < 34)
                return -1;
            int nl = sec[p + 32];
            if (33 + nl <= len && name_eq(sec + p + 33, nl, name, nlen)) {
                *out_lba = rd32(sec + p + 2);
                *out_size = rd32(sec + p + 10);
                *is_dir = (sec[p + 25] & 2) != 0;
                return 0;
            }
            p += len;
        }
    }
    return -1;
}

int disc_find(Disc *d, const char *path, uint32_t *offset, uint32_t *size)
{
    uint8_t pvd[SECTOR];
    if (disc_read(d, pvd, SECTOR, 16 * SECTOR) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5))
        return -1;
    uint32_t lba = rd32(pvd + 156 + 2), sz = rd32(pvd + 156 + 10);
    int is_dir = 1;
    while (*path) {
        const char *slash = strchr(path, '/');
        int nlen = slash ? (int)(slash - path) : (int)strlen(path);
        if (!is_dir || find_in_dir(d, lba, sz, path, nlen, &lba, &sz, &is_dir))
            return -1;
        path += nlen + (slash ? 1 : 0);
    }
    if (is_dir)
        return -1;
    *offset = lba * SECTOR;
    *size = sz;
    return 0;
}
