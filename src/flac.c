/* XLT - FLAC decoder. See flac.h.
 *
 * Frames are found by their sync code and header checksum, so a damaged stretch
 * only costs the frames in it. Seeking narrows down by the sample numbers in the
 * frame headers (no SEEKTABLE needed), then scans to the frame holding the sample.
 * Every length comes from the file and is checked before use; arithmetic on
 * samples is unsigned so a corrupt frame gives wrong sound, not undefined behaviour. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "flac.h"
#include "xio.h"

#define BUF_SIZE 32768
#define KEEP 8           /* bytes kept before pos, for bits read ahead */
#define HEADER_MAX 16
#define NARROW (64 * 1024)
#define MAX_TAGS (256 * 1024)

static uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | p[1] << 8 | p[2]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Bytes of an ID3v2 tag at the start of the file (some taggers add one). */
static uint32_t id3_skip(const uint8_t h[10])
{
    if (memcmp(h, "ID3", 3))
        return 0;
    return 10 + ((h[6] & 0x7Fu) << 21 | (h[7] & 0x7Fu) << 14 | (h[8] & 0x7Fu) << 7 | (h[9] & 0x7Fu));
}

/* ---------------------------------------------------------------- reading */

static void go_to(Flac *f, uint64_t off)
{
    f->buf_off = off;
    f->pos = f->len = 0;
    f->cache = 0;
    f->nbits = f->overrun = 0;
}

/* At least `want` bytes after pos, unless the file ends first. */
static void ensure(Flac *f, uint32_t want)
{
    if (f->len - f->pos >= want)
        return;
    uint32_t keep = f->pos < KEEP ? f->pos : KEEP, drop = f->pos - keep;
    memmove(f->buf, f->buf + drop, f->len - drop);
    f->len -= drop;
    f->pos -= drop;
    f->buf_off += drop;
    int got = f->read(f->ctx, f->buf_off + f->len, f->buf + f->len, BUF_SIZE - f->len);
    if (got > 0)
        f->len += (uint32_t)got;
}

static inline void fill(Flac *f)
{
    if (f->pos >= f->len) {
        ensure(f, 1);
        if (f->pos >= f->len) { /* past the end: zeros, and the frame is thrown away */
            f->overrun = 1;
            f->nbits += 8;
            return;
        }
    }
    f->cache |= (uint32_t)f->buf[f->pos++] << (24 - f->nbits);
    f->nbits += 8;
}

static inline uint32_t bits(Flac *f, int k)
{
    if (k == 0)
        return 0;
    if (k > 24) {
        uint32_t hi = bits(f, k - 16);
        return hi << 16 | bits(f, 16);
    }
    while (f->nbits < k)
        fill(f);
    uint32_t v = f->cache >> (32 - k);
    f->cache <<= k;
    f->nbits -= k;
    return v;
}

static inline int32_t sbits(Flac *f, int k)
{
    if (k == 0)
        return 0;
    uint32_t v = bits(f, k);
    return k < 32 && (v >> (k - 1)) ? (int32_t)(v | (~0u << k)) : (int32_t)v;
}

/* Zero bits up to the next one, which is used up too. */
static inline uint32_t unary(Flac *f)
{
    uint32_t q = 0;
    for (;;) {
        if (f->cache) {
            int z = __builtin_clz(f->cache);
            if (z < f->nbits) {
                f->cache = (f->cache << z) << 1;
                f->nbits -= z + 1;
                return q + (uint32_t)z;
            }
        }
        q += (uint32_t)f->nbits;
        f->nbits = 0;
        f->cache = 0;
        fill(f);
        if (f->overrun || q > (1u << 24)) {
            f->overrun = 1;
            return q;
        }
    }
}

/* -------------------------------------------------------------- the frames */

typedef struct {
    int block, rate, assign, bps, len;
    uint64_t number;
    int variable;
} Header;

static uint8_t crc8(const uint8_t *p, int n)
{
    uint8_t c = 0;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = (uint8_t)(c & 0x80 ? c << 1 ^ 0x07 : c << 1);
    }
    return c;
}

/* A frame header at p (HEADER_MAX bytes readable), checked against the stream. 1 if valid. */
static int parse_header(const Flac *f, const uint8_t *p, Header *h)
{
    if (p[0] != 0xFF || (p[1] & 0xFE) != 0xF8)
        return 0;
    static const int rates[12] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    static const int depths[8] = {0, 8, 12, -1, 16, 20, 24, 32};
    int bc = p[2] >> 4, rc = p[2] & 15, ac = p[3] >> 4, dc = p[3] >> 1 & 7;
    if (bc == 0 || rc == 15 || ac > 10 || depths[dc] < 0 || (p[3] & 1))
        return 0;
    int i = 4;
    /* the UTF-8-style frame or sample number */
    uint64_t num = p[i++];
    int extra = 0;
    if (num >= 0xFE) { extra = num == 0xFF ? -1 : 6; num = 0; }
    else if (num >= 0xFC) { extra = 5; num &= 1; }
    else if (num >= 0xF8) { extra = 4; num &= 3; }
    else if (num >= 0xF0) { extra = 3; num &= 7; }
    else if (num >= 0xE0) { extra = 2; num &= 15; }
    else if (num >= 0xC0) { extra = 1; num &= 31; }
    else if (num >= 0x80) return 0;
    if (extra < 0)
        return 0;
    for (; extra > 0; extra--) {
        if ((p[i] & 0xC0) != 0x80)
            return 0;
        num = num << 6 | (p[i++] & 0x3F);
    }
    int block;
    if (bc == 1) block = 192;
    else if (bc <= 5) block = 576 << (bc - 2);
    else if (bc == 6) block = p[i++] + 1;
    else if (bc == 7) { block = (p[i] << 8 | p[i + 1]) + 1; i += 2; }
    else block = 256 << (bc - 8);
    int rate;
    if (rc == 0) rate = f->rate;
    else if (rc < 12) rate = rates[rc];
    else if (rc == 12) rate = p[i++] * 1000;
    else if (rc == 13) { rate = p[i] << 8 | p[i + 1]; i += 2; }
    else { rate = (p[i] << 8 | p[i + 1]) * 10; i += 2; }
    if (crc8(p, i) != p[i])
        return 0;
    i++;
    int bps = dc ? depths[dc] : f->bps;
    int channels = ac < 8 ? ac + 1 : 2;
    if (rate != f->rate || bps != f->bps || channels != f->channels || block > f->alloc_block)
        return 0;
    h->block = block;
    h->rate = rate;
    h->assign = ac;
    h->bps = bps;
    h->len = i;
    h->number = num;
    h->variable = p[1] & 1;
    return 1;
}

static uint64_t first_sample(const Flac *f, const Header *h)
{
    return h->variable ? h->number : h->number * (uint64_t)f->min_block;
}

/* Moves pos to the next valid header. 1 if found, 0 at the end of the file. */
static int find_header(Flac *f, Header *h)
{
    for (;;) {
        ensure(f, HEADER_MAX + 1);
        if (f->len - f->pos < 6)
            return 0;
        if (f->buf[f->pos] == 0xFF) {
            /* near the end of the file the bytes past it are zero: buf has room */
            memset(f->buf + f->len, 0, BUF_SIZE - f->len < HEADER_MAX ? BUF_SIZE - f->len : HEADER_MAX);
            if (parse_header(f, f->buf + f->pos, h))
                return 1;
        }
        f->pos++;
    }
}

static int residual(Flac *f, int32_t *out, int block, int order)
{
    int method = (int)bits(f, 2);
    if (method > 1)
        return -1;
    int pbits = method ? 5 : 4, escape = (1 << pbits) - 1;
    int po = (int)bits(f, 4), parts = 1 << po;
    int per = block >> po;
    if ((per << po) != block || per < order)
        return -1;
    int i = order;
    for (int p = 0; p < parts; p++) {
        int n = per - (p == 0 ? order : 0);
        int k = (int)bits(f, pbits);
        if (k == escape) {
            int raw = (int)bits(f, 5);
            for (int j = 0; j < n; j++)
                out[i++] = sbits(f, raw);
        } else {
            for (int j = 0; j < n; j++) {
                uint32_t q = unary(f);
                uint32_t u = q << k | bits(f, k);
                out[i++] = (int32_t)(u >> 1 ^ (0u - (u & 1)));
            }
        }
    }
    return f->overrun ? -1 : 0;
}

static void fixed_predict(int32_t *s, int block, int order)
{
    uint32_t *u = (uint32_t *)s;
    switch (order) {
    case 1: for (int i = 1; i < block; i++) u[i] += u[i - 1]; break;
    case 2: for (int i = 2; i < block; i++) u[i] += 2 * u[i - 1] - u[i - 2]; break;
    case 3: for (int i = 3; i < block; i++) u[i] += 3 * u[i - 1] - 3 * u[i - 2] + u[i - 3]; break;
    case 4: for (int i = 4; i < block; i++) u[i] += 4 * u[i - 1] - 6 * u[i - 2] + 4 * u[i - 3] - u[i - 4]; break;
    default: break;
    }
}

static void lpc32(int32_t *s, int block, const int32_t *c, int order, int shift)
{
    uint32_t *u = (uint32_t *)s;
    for (int i = order; i < block; i++) {
        uint32_t sum = 0;
        for (int j = 0; j < order; j++)
            sum += (uint32_t)c[j] * u[i - 1 - j];
        u[i] += (uint32_t)((int32_t)sum >> shift);
    }
}

static void lpc64(int32_t *s, int block, const int32_t *c, int order, int shift)
{
    uint32_t *u = (uint32_t *)s;
    for (int i = order; i < block; i++) {
        uint64_t sum = 0;
        for (int j = 0; j < order; j++)
            sum += (uint64_t)((int64_t)c[j] * s[i - 1 - j]);
        u[i] += (uint32_t)((int64_t)sum >> shift);
    }
}

static int ilog2(int v) { return v ? 31 - __builtin_clz((unsigned)v) : 0; }

static int subframe(Flac *f, int32_t *s, int block, int bps)
{
    if (bits(f, 1)) /* the padding bit */
        return -1;
    int type = (int)bits(f, 6);
    int wasted = 0;
    if (bits(f, 1))
        wasted = (int)unary(f) + 1;
    if (wasted >= bps)
        return -1;
    bps -= wasted;
    if (type == 0) {
        int32_t v = sbits(f, bps);
        for (int i = 0; i < block; i++)
            s[i] = v;
    } else if (type == 1) {
        for (int i = 0; i < block; i++)
            s[i] = sbits(f, bps);
    } else if (type >= 8 && type <= 12) {
        int order = type - 8;
        if (order > block)
            return -1;
        for (int i = 0; i < order; i++)
            s[i] = sbits(f, bps);
        if (residual(f, s, block, order))
            return -1;
        fixed_predict(s, block, order);
    } else if (type >= 32) {
        int order = type - 31;
        if (order > block)
            return -1;
        for (int i = 0; i < order; i++)
            s[i] = sbits(f, bps);
        int prec = (int)bits(f, 4) + 1;
        int shift = sbits(f, 5);
        if (prec == 16 || shift < 0)
            return -1;
        int32_t c[32];
        for (int i = 0; i < order; i++)
            c[i] = sbits(f, prec);
        if (residual(f, s, block, order))
            return -1;
        if (bps + prec + ilog2(order) + 1 <= 32)
            lpc32(s, block, c, order, shift);
        else
            lpc64(s, block, c, order, shift);
    } else {
        return -1;
    }
    if (wasted)
        for (int i = 0; i < block; i++)
            s[i] = (int32_t)((uint32_t)s[i] << wasted);
    return f->overrun ? -1 : 0;
}

static void decorrelate(Flac *f, const Header *h)
{
    uint32_t *a = (uint32_t *)f->s[0], *b = (uint32_t *)f->s[1];
    switch (h->assign) {
    case 8: /* left, side */
        for (int i = 0; i < h->block; i++) b[i] = a[i] - b[i];
        break;
    case 9: /* side, right */
        for (int i = 0; i < h->block; i++) a[i] += b[i];
        break;
    case 10: /* mid, side */
        for (int i = 0; i < h->block; i++) {
            uint32_t mid = a[i] << 1 | (b[i] & 1);
            int32_t l = (int32_t)(mid + b[i]) >> 1, r = (int32_t)(mid - b[i]) >> 1;
            a[i] = (uint32_t)l;
            b[i] = (uint32_t)r;
        }
        break;
    default:
        break;
    }
}

/* The frame whose header is at pos. 0, or -1 if its contents are bad. */
static int read_frame(Flac *f, const Header *h)
{
    f->pos += (uint32_t)h->len;
    f->cache = 0;
    f->nbits = 0;
    f->overrun = 0;
    for (int ch = 0; ch < f->channels; ch++) {
        int bps = h->bps;
        if ((h->assign == 8 && ch == 1) || (h->assign == 9 && ch == 0) || (h->assign == 10 && ch == 1))
            bps++; /* the side channel takes one bit more */
        if (subframe(f, f->s[ch], h->block, bps))
            return -1;
    }
    decorrelate(f, h);
    /* the rest of the byte, then the bytes read ahead go back; the CRC-16 isn't checked */
    bits(f, f->nbits & 7);
    f->pos -= (uint32_t)f->nbits / 8 > f->pos ? f->pos : (uint32_t)f->nbits / 8;
    f->cache = 0;
    f->nbits = 0;
    ensure(f, 2);
    f->pos = f->pos + 2 > f->len ? f->len : f->pos + 2;
    return 0;
}

int flac_decode(Flac *f)
{
    Header h;
    for (;;) {
        if (!find_header(f, &h))
            return 0;
        uint64_t at = first_sample(f, &h);
        if (f->total && at >= f->total)
            return 0;
        uint64_t off = f->buf_off + f->pos;
        if (read_frame(f, &h) == 0) {
            f->block = h.block;
            f->frame_sample = at;
            return h.block;
        }
        go_to(f, off + 1); /* a bad frame: on to the next one that is whole */
    }
}

int flac_seek(Flac *f, uint64_t sample)
{
    uint64_t lo = f->data_start, hi = f->file_size;
    Header h;
    while (hi - lo > NARROW) {
        uint64_t mid = lo + (hi - lo) / 2;
        go_to(f, mid);
        if (!find_header(f, &h) || f->buf_off + f->pos >= hi) {
            hi = mid;
            continue;
        }
        uint64_t off = f->buf_off + f->pos;
        if (first_sample(f, &h) <= sample) lo = off;
        else hi = mid;
    }
    go_to(f, lo);
    for (;;) { /* the frame holding it: headers only, nothing to decode on the way */
        if (!find_header(f, &h))
            return FLAC_ERR_DATA;
        uint64_t at = first_sample(f, &h);
        if (at + (uint64_t)h.block > sample) {
            f->skip = at < sample ? (uint32_t)(sample - at) : 0;
            return 0;
        }
        f->pos++;
    }
}

int flac_output(Flac *f, int16_t *dst)
{
    int n = 0;
    int shift_r = f->bps > 16 ? f->bps - 16 : 0, shift_l = f->bps < 16 ? 16 - f->bps : 0;
    const int32_t *l = f->s[0], *r = f->s[f->channels > 1];
    for (int i = (int)f->skip; i < f->block; i += f->step) {
        if (shift_r) {
            dst[n * 2] = (int16_t)(l[i] >> shift_r);
            dst[n * 2 + 1] = (int16_t)(r[i] >> shift_r);
        } else {
            dst[n * 2] = (int16_t)((uint32_t)l[i] << shift_l);
            dst[n * 2 + 1] = (int16_t)((uint32_t)r[i] << shift_l);
        }
        n++;
    }
    f->skip = 0;
    return n;
}

int flac_duration_ms(const Flac *f)
{
    return f->rate ? (int)(f->total * 1000 / (uint64_t)f->rate) : 0;
}

/* ---------------------------------------------------------------- opening */

static int psp_rate(int r)
{
    static const int ok[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        if (ok[i] == r)
            return 1;
    return 0;
}

void flac_close(Flac *f)
{
    free(f->buf);
    free(f->mem);
    f->buf = NULL;
    f->mem = NULL;
}

int flac_open(Flac *f, FlacReadFn read, void *ctx, uint64_t file_size)
{
    memset(f, 0, sizeof(*f));
    f->read = read;
    f->ctx = ctx;
    f->file_size = file_size;
    uint8_t h[42];
    if (read(ctx, 0, h, 10) != 10)
        return FLAC_ERR_READ;
    uint64_t at = id3_skip(h);
    if (read(ctx, at, h, 42) != 42)
        return FLAC_ERR_FORMAT;
    if (memcmp(h, "fLaC", 4) || (h[4] & 0x7F) != 0 || be24(h + 5) < 34)
        return FLAC_ERR_FORMAT;
    const uint8_t *si = h + 8;
    f->min_block = si[0] << 8 | si[1];
    f->max_block = si[2] << 8 | si[3];
    f->rate = (int)((uint32_t)si[10] << 12 | si[11] << 4 | si[12] >> 4);
    f->channels = (si[12] >> 1 & 7) + 1;
    f->bps = ((si[12] & 1) << 4 | si[13] >> 4) + 1;
    f->total = (uint64_t)(si[13] & 15) << 32 | be32(si + 14);
    if (f->rate <= 0 || f->min_block < 16 || f->bps < 4)
        return FLAC_ERR_FORMAT;
    f->step = 1;
    f->out_rate = f->rate;
    while (f->out_rate > 48000) { /* hi-res: the PSP stops at 48 kHz */
        f->out_rate /= 2;
        f->step *= 2;
    }
    if (!psp_rate(f->out_rate))
        return FLAC_ERR_RATE;
    /* past the metadata blocks */
    uint8_t b[4];
    at += 4;
    for (;;) {
        if (read(ctx, at, b, 4) != 4)
            return FLAC_ERR_FORMAT;
        at += 4 + (uint64_t)be24(b + 1);
        if (b[0] & 0x80)
            break;
        if (at >= file_size)
            return FLAC_ERR_FORMAT;
    }
    f->data_start = at;
    f->alloc_block = f->max_block > 4096 ? f->max_block : 4096;
    f->buf = malloc(BUF_SIZE + HEADER_MAX);
    f->mem = malloc((size_t)f->alloc_block * (size_t)f->channels * sizeof(int32_t));
    if (!f->buf || !f->mem) {
        flac_close(f);
        return FLAC_ERR_NO_MEM;
    }
    for (int i = 0; i < f->channels; i++)
        f->s[i] = f->mem + (size_t)i * (size_t)f->alloc_block;
    go_to(f, f->data_start);
    return 0;
}

/* ------------------------------------------------------------------- tags */

/* Value of a "KEY=value" comment if its key is key, else NULL. */
static const char *comment_is(const char *c, uint32_t n, const char *key, uint32_t *vlen)
{
    uint32_t k = (uint32_t)strlen(key);
    if (n <= k || c[k] != '=' || strncasecmp(c, key, k))
        return NULL;
    *vlen = n - k - 1;
    return c + k + 1;
}

static void copy_value(char *out, int cap, const char *v, uint32_t n)
{
    uint32_t m = n < (uint32_t)cap - 1 ? n : (uint32_t)cap - 1;
    while (m > 0 && m < n && ((unsigned char)v[m] & 0xC0) == 0x80) /* not in the middle of a letter */
        m--;
    memcpy(out, v, m);
    out[m] = 0;
}

typedef void (*CommentFn)(const char *c, uint32_t n, void *ctx);

static void walk_comments(const uint8_t *p, uint32_t size, CommentFn fn, void *ctx)
{
    if (size < 8)
        return;
    uint32_t vendor = le32(p), at = 4;
    if (vendor > size - 8)
        return;
    at += vendor;
    uint32_t count = le32(p + at);
    at += 4;
    for (uint32_t i = 0; i < count && size - at >= 4; i++) {
        uint32_t n = le32(p + at);
        at += 4;
        if (n > size - at)
            return;
        fn((const char *)p + at, n, ctx);
        at += n;
    }
}

static void take_tag(const char *c, uint32_t n, void *ctx)
{
    Id3Info *o = ctx;
    const char *v;
    uint32_t len;
    if ((v = comment_is(c, n, "TITLE", &len))) copy_value(o->title, sizeof(o->title), v, len);
    else if ((v = comment_is(c, n, "ARTIST", &len))) copy_value(o->artist, sizeof(o->artist), v, len);
    else if ((v = comment_is(c, n, "ALBUM", &len))) copy_value(o->album, sizeof(o->album), v, len);
    else if ((v = comment_is(c, n, "TRACKNUMBER", &len))) {
        int t = 0;
        for (uint32_t i = 0; i < len && v[i] >= '0' && v[i] <= '9' && t < 100000; i++)
            t = t * 10 + (v[i] - '0');
        o->track = t;
    } else if (n > 15 && (!strncasecmp(c, "REPLAYGAIN_", 11))) {
        const char *eq = memchr(c, '=', n);
        if (eq && eq - c < 32) {
            char key[32], val[32];
            snprintf(key, sizeof(key), "%.*s", (int)(eq - c), c);
            snprintf(val, sizeof(val), "%.*s", (int)(n - (uint32_t)(eq - c) - 1 < 31 ? n - (uint32_t)(eq - c) - 1 : 31), eq + 1);
            id3_take_gain(o, key, val);
        }
    }
}

typedef struct { char *out; int cap, found; } Lyr;

static void take_lyrics(const char *c, uint32_t n, void *ctx)
{
    Lyr *l = ctx;
    const char *v;
    uint32_t len;
    if (l->found)
        return;
    if ((v = comment_is(c, n, "LYRICS", &len)) || (v = comment_is(c, n, "UNSYNCEDLYRICS", &len)) ||
        (v = comment_is(c, n, "UNSYNCED LYRICS", &len))) {
        copy_value(l->out, l->cap, v, len);
        l->found = len > 0;
    }
}

/* Walks the metadata blocks of an open file. The comment block is handed to fn. */
static int walk_meta(int fd, CommentFn fn, void *ctx, Id3Info *art)
{
    uint8_t h[10];
    if (xio_read_at(fd, h, 10, 0) != 10)
        return -1;
    uint64_t at = id3_skip(h);
    if (xio_read_at(fd, h, 4, at) != 4 || memcmp(h, "fLaC", 4))
        return -1;
    at += 4;
    int have_front = 0;
    for (int guard = 0; guard < 1000; guard++) {
        uint8_t b[4];
        if (xio_read_at(fd, b, 4, at) != 4)
            return guard ? 0 : -1;
        uint32_t type = b[0] & 0x7F, size = be24(b + 1);
        uint64_t body = at + 4;
        if (type == 4 && size <= MAX_TAGS) {
            uint8_t *p = malloc(size ? size : 1);
            if (p && xio_read_at(fd, p, size, body) == (int)size)
                walk_comments(p, size, fn, ctx);
            free(p);
        } else if (type == 6 && art && !have_front && size >= 32) {
            /* type, MIME, description, 4 numbers, then the picture */
            uint8_t q[8];
            if (xio_read_at(fd, q, 8, body) == 8) {
                uint32_t pic = be32(q), mime = be32(q + 4), desc;
                uint8_t d[4];
                if (mime < size && xio_read_at(fd, d, 4, body + 8 + mime) == 4) {
                    desc = be32(d);
                    uint64_t lenat = body + 12 + (uint64_t)mime + desc + 16;
                    uint8_t l[4];
                    if (desc < size && lenat + 4 <= body + size && xio_read_at(fd, l, 4, lenat) == 4) {
                        uint32_t n = be32(l);
                        if (n && lenat + 4 + (uint64_t)n <= body + size && (!art->art_size || pic == 3)) {
                            art->art_off = (uint32_t)(lenat + 4);
                            art->art_size = n;
                            have_front = pic == 3;
                        }
                    }
                }
            }
        }
        at = body + size;
        if (b[0] & 0x80)
            break;
    }
    return 0;
}

int flac_tags(const char *path, Id3Info *out)
{
    memset(out, 0, sizeof(*out));
    int fd = xio_open(path);
    if (fd < 0)
        return -1;
    int r = walk_meta(fd, take_tag, out, out);
    xio_close(fd);
    return r;
}

int flac_lyrics(const char *path, char *out, int cap)
{
    int fd = xio_open(path);
    if (fd < 0)
        return -1;
    Lyr l = {out, cap, 0};
    out[0] = 0;
    walk_meta(fd, take_lyrics, &l, NULL);
    xio_close(fd);
    return l.found ? 0 : -1;
}
