/* XLT - ID3 parser. See id3.h. */
#include <stdlib.h>
#include <string.h>
#include "id3.h"
#include "xio.h"

#define TEXT_MAX 512 /* longest text frame we read */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | p[1] << 8 | p[2]; }
static uint32_t syncsafe(const uint8_t *p) { return (p[0] & 0x7F) << 21 | (p[1] & 0x7F) << 14 | (p[2] & 0x7F) << 7 | (p[3] & 0x7F); }

/* Appends code point c as UTF-8. */
static int put_utf8(char *o, int pos, int cap, unsigned c)
{
    char b[4];
    int n;
    if (c < 0x80) { b[0] = c; n = 1; }
    else if (c < 0x800) { b[0] = 0xC0 | c >> 6; b[1] = 0x80 | (c & 0x3F); n = 2; }
    else { b[0] = 0xE0 | c >> 12; b[1] = 0x80 | ((c >> 6) & 0x3F); b[2] = 0x80 | (c & 0x3F); n = 3; }
    if (pos + n >= cap) return pos;
    memcpy(o + pos, b, n);
    return pos + n;
}

/* Decodes an ID3 text payload (encoding byte first) to UTF-8 in out. */
static void decode_text(const uint8_t *p, int n, char *out, int cap)
{
    int o = 0;
    out[0] = 0;
    if (n < 1) return;
    int enc = p[0];
    p++; n--;
    if (enc == 1 || enc == 2) { /* UTF-16 with BOM, or UTF-16BE */
        int be = enc == 2;
        if (n >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF))) {
            be = p[0] == 0xFE;
            p += 2; n -= 2;
        }
        for (int i = 0; i + 1 < n; i += 2) {
            unsigned c = be ? (p[i] << 8 | p[i + 1]) : (p[i] | p[i + 1] << 8);
            if (c == 0) break;
            if (c >= 0xD800 && c <= 0xDFFF) c = '?'; /* outside the BMP: not in our font anyway */
            o = put_utf8(out, o, cap, c);
        }
    } else if (enc == 3) { /* UTF-8 */
        for (int i = 0; i < n && p[i] && o < cap - 1; i++) out[o++] = p[i];
    } else { /* ISO-8859-1 */
        for (int i = 0; i < n && p[i]; i++) o = put_utf8(out, o, cap, p[i]);
    }
    out[o] = 0;
    /* trim trailing spaces (ID3v1 pads with them) */
    while (o > 0 && (out[o - 1] == ' ')) out[--o] = 0;
}

static int track_number(const char *s)
{
    int n = 0;
    while (*s >= '0' && *s <= '9' && n < 10000) n = n * 10 + (*s++ - '0');
    return n;
}

/* Skips an encoded, NUL-terminated string in p[0..n); returns bytes used. */
static int skip_string(const uint8_t *p, int n, int enc)
{
    int i = 0;
    if (enc == 1 || enc == 2) {
        while (i + 1 < n && (p[i] || p[i + 1])) i += 2;
        return i + 2 <= n ? i + 2 : n;
    }
    while (i < n && p[i]) i++;
    return i < n ? i + 1 : n;
}

static int is_image(const uint8_t *p, int n)
{
    return (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) || (n >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8));
}

/* APIC (v2.3/2.4) or PIC (v2.2): find where the image data begins. */
static void picture(int fd, uint32_t off, uint32_t size, int v22, Id3Info *o, int *have_front)
{
    uint8_t b[300];
    int n = size < sizeof(b) ? (int)size : (int)sizeof(b);
    if (xio_read_at(fd, b, n, off) != n || n < 4) return;
    int enc = b[0], p = 1, type;
    if (v22) {
        p += 3; /* image format, e.g. "JPG" */
    } else {
        while (p < n && b[p]) p++; /* MIME type */
        p++;
    }
    if (p >= n) return;
    type = b[p++];
    p += skip_string(b + p, n - p, enc); /* description */
    if (p >= n || !is_image(b + p, n - p)) return;
    int front = type == 3;
    if (o->art_size && (*have_front || !front))
        return; /* keep the first picture unless this one is the front cover */
    o->art_off = off + p;
    o->art_size = size - p;
    *have_front = front;
}

/* "-7.23 dB", "+0.5", "-7,23 dB" -> hundredths of a dB. 0 if it is a number. */
static int parse_db(const char *s, int *out)
{
    while (*s == ' ') s++;
    int neg = *s == '-';
    if (*s == '-' || *s == '+') s++;
    if (*s < '0' || *s > '9') return -1;
    long v = 0;
    while (*s >= '0' && *s <= '9' && v < 100000) v = v * 10 + (*s++ - '0');
    v *= 100;
    if (*s == '.' || *s == ',') {
        s++;
        if (*s >= '0' && *s <= '9') { v += (*s++ - '0') * 10;
            if (*s >= '0' && *s <= '9') { v += *s++ - '0';
                if (*s >= '5' && *s <= '9') v++; } }
        while (*s >= '0' && *s <= '9') s++;
    }
    if (v > 6000) return -1; /* +-60 dB: nonsense */
    *out = (int)(neg ? -v : v);
    return 0;
}

/* iTunNORM: " 0000031C 00000307 ..." in hex; the first two are the left and right
 * loudness in milliwatts against 1000 (pairs 3-4 are the same against 2500). The
 * louder one sets the gain: -10 log10(v / 1000) dB. */
static int parse_itunnorm(const char *s, int *out)
{
    unsigned long v[2] = {0, 0};
    for (int k = 0; k < 2; k++) {
        while (*s == ' ') s++;
        int digits = 0;
        unsigned long x = 0;
        for (;; s++, digits++) {
            int c = *s, d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0 || digits >= 8) break;
            x = x << 4 | (unsigned long)d;
        }
        if (!digits) return -1;
        v[k] = x;
    }
    unsigned long m = v[0] > v[1] ? v[0] : v[1];
    if (!m) return -1;
    /* -1000 log10(m / 1000) in hundredths of a dB, without libm: log10 by halving */
    double r = (double)m / 1000.0, l = 0;
    int e = 0;
    while (r >= 2.0) { r /= 2.0; e++; }
    while (r < 1.0) { r *= 2.0; e--; }
    /* ln(r) for r in [1,2) by the atanh series */
    double z = (r - 1) / (r + 1), z2 = z * z, t = z, ln = 0;
    for (int i = 1; i < 30; i += 2) { ln += t / i; t *= z2; }
    ln *= 2;
    l = (ln + e * 0.69314718055994531) / 2.30258509299404568; /* log10 */
    double g = -1000.0 * l;
    if (g > 6000 || g < -6000) return -1;
    *out = (int)(g < 0 ? g - 0.5 : g + 0.5);
    return 0;
}

static int same_key(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return 0;
    }
    return !*a && !*b;
}

void id3_take_gain(Id3Info *o, const char *key, const char *value)
{
    int g;
    if (same_key(key, "replaygain_track_gain") && parse_db(value, &g) == 0) { o->gain_track = g; o->gains |= GAIN_TRACK; }
    else if (same_key(key, "replaygain_album_gain") && parse_db(value, &g) == 0) { o->gain_album = g; o->gains |= GAIN_ALBUM; }
    else if (same_key(key, "itunnorm") && parse_itunnorm(value, &g) == 0) { o->gain_sc = g; o->gains |= GAIN_SC; }
}

/* TXXX (description, value) and COMM (language, description, text): the loudness tags. */
static void gain_frame(const uint8_t *t, int n, int comm, Id3Info *o)
{
    if (n < 2 + (comm ? 3 : 0)) return;
    int enc = t[0], at = 1 + (comm ? 3 : 0);
    int dl = skip_string(t + at, n - at, enc);
    uint8_t d[66], v[66];
    int dn = dl < 65 ? dl : 65, vn = n - at - dl < 65 ? n - at - dl : 65;
    if (vn <= 0) return;
    d[0] = v[0] = (uint8_t)enc;
    memcpy(d + 1, t + at, dn);
    memcpy(v + 1, t + at + dl, vn);
    char key[40], val[80];
    decode_text(d, dn + 1, key, sizeof(key));
    decode_text(v, vn + 1, val, sizeof(val));
    id3_take_gain(o, key, val);
}

static void read_v2(int fd, Id3Info *o)
{
    uint8_t h[10];
    if (xio_read_at(fd, h, 10, 0) != 10 || memcmp(h, "ID3", 3) || h[3] < 2 || h[3] > 4)
        return;
    int ver = h[3], flags = h[5];
    uint32_t tag = syncsafe(h + 6), end = 10 + tag;
    o->audio_start = end + ((ver == 4 && (flags & 0x10)) ? 10 : 0); /* footer */
    int unsync = flags & 0x80; /* whole-tag unsynchronisation (v2.2/2.3): offsets would be off */
    uint32_t pos = 10;
    if (ver >= 3 && (flags & 0x40)) { /* extended header */
        uint8_t e[4];
        if (xio_read_at(fd, e, 4, pos) != 4) return;
        pos += ver == 4 ? syncsafe(e) : be32(e) + 4;
    }
    int idlen = ver == 2 ? 3 : 4, hlen = ver == 2 ? 6 : 10, have_front = 0;
    while (pos + hlen <= end) {
        uint8_t f[10];
        if (xio_read_at(fd, f, hlen, pos) != hlen || f[0] == 0)
            break; /* padding */
        uint32_t size = ver == 2 ? be24(f + 3) : ver == 4 ? syncsafe(f + 4) : be32(f + 4);
        uint32_t data = pos + hlen;
        if (size == 0 || data + size > end)
            break;
        int fflags = ver >= 3 ? f[9] : 0;
        /* v2.4 per-frame flags: compressed/encrypted/unsynchronised data is skipped */
        int unreadable = ver == 4 ? (fflags & 0x0F) != 0 : ver == 3 ? (fflags & 0xC0) != 0 : 0;
        char id[5] = {0};
        memcpy(id, f, idlen);
        if (!unreadable) {
            char *dst = NULL;
            int cap = 0, trk = 0;
            if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) { dst = o->title; cap = sizeof(o->title); }
            else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) { dst = o->artist; cap = sizeof(o->artist); }
            else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) { dst = o->album; cap = sizeof(o->album); }
            else if (!strcmp(id, "TRCK") || !strcmp(id, "TRK")) trk = 1;
            if (dst || trk) {
                uint8_t t[TEXT_MAX];
                int n = size < TEXT_MAX ? (int)size : TEXT_MAX;
                if (xio_read_at(fd, t, n, data) == n) {
                    char tmp[64];
                    if (trk) {
                        decode_text(t, n, tmp, sizeof(tmp));
                        o->track = track_number(tmp);
                    } else {
                        decode_text(t, n, dst, cap);
                    }
                }
            } else if (!strcmp(id, "TXXX") || !strcmp(id, "TXX") || !strcmp(id, "COMM") || !strcmp(id, "COM")) {
                uint8_t t[200];
                int n = size < sizeof(t) ? (int)size : (int)sizeof(t);
                if (size <= 200 && xio_read_at(fd, t, n, data) == n) /* a long comment isn't one of these */
                    gain_frame(t, n, id[1] == 'O', o);
            } else if (!unsync && (!strcmp(id, "APIC") || !strcmp(id, "PIC"))) {
                picture(fd, data, size, ver == 2, o, &have_front);
            }
        }
        pos = data + size;
    }
}

static void read_v1(int fd, uint64_t file_size, Id3Info *o)
{
    if (file_size < 128) return;
    uint8_t t[128];
    if (xio_read_at(fd, t, 128, file_size - 128) != 128 || memcmp(t, "TAG", 3)) return;
    uint8_t buf[32];
    struct { int at; char *dst; int cap; } f[3] = {
        {3, o->title, sizeof(o->title)}, {33, o->artist, sizeof(o->artist)}, {63, o->album, sizeof(o->album)}};
    for (int i = 0; i < 3; i++) {
        if (f[i].dst[0]) continue; /* v2 wins */
        buf[0] = 0; /* ISO-8859-1 */
        memcpy(buf + 1, t + f[i].at, 30);
        decode_text(buf, 31, f[i].dst, f[i].cap);
    }
    if (!o->track && t[125] == 0 && t[126])
        o->track = t[126];
}

int id3_read(const char *path, uint64_t file_size, Id3Info *o)
{
    memset(o, 0, sizeof(*o));
    int fd = xio_open(path);
    if (fd < 0)
        return -1;
    read_v2(fd, o);
    read_v1(fd, file_size, o);
    xio_close(fd);
    return 0;
}

/* ------------------------------------------------------------------ lyrics */

int id3_lyrics(const char *path, char *out, int cap)
{
    out[0] = 0;
    int fd = xio_open(path);
    if (fd < 0)
        return -1;
    uint8_t h[10];
    int found = -1;
    if (xio_read_at(fd, h, 10, 0) == 10 && !memcmp(h, "ID3", 3) && h[3] >= 2 && h[3] <= 4 && !(h[5] & 0x80)) {
        int ver = h[3];
        uint32_t end = 10 + syncsafe(h + 6), pos = 10;
        if (ver >= 3 && (h[5] & 0x40)) { /* extended header */
            uint8_t e[4];
            if (xio_read_at(fd, e, 4, pos) == 4)
                pos += ver == 4 ? syncsafe(e) : be32(e) + 4;
        }
        int idlen = ver == 2 ? 3 : 4, hlen = ver == 2 ? 6 : 10;
        while (found < 0 && pos + hlen <= end) {
            uint8_t f[10];
            if (xio_read_at(fd, f, hlen, pos) != hlen || f[0] == 0)
                break;
            uint32_t size = ver == 2 ? be24(f + 3) : ver == 4 ? syncsafe(f + 4) : be32(f + 4), data = pos + hlen;
            if (size == 0 || data + size > end)
                break;
            int fflags = ver >= 3 ? f[9] : 0;
            int unreadable = ver == 4 ? (fflags & 0x0F) != 0 : ver == 3 ? (fflags & 0xC0) != 0 : 0;
            if (!unreadable && (!memcmp(f, "USLT", idlen) || (ver == 2 && !memcmp(f, "ULT", 3))) && size > 5) {
                /* encoding, language (3), description (NUL-ended), then the text in the same encoding */
                int n = size < 16384 ? (int)size : 16384;
                uint8_t *b = malloc(n);
                if (b && xio_read_at(fd, b, n, data) == n) {
                    int enc = b[0], p = 4;
                    p += skip_string(b + p, n - p, enc);
                    if (p < n) {
                        b[p - 1] = (uint8_t)enc; /* decode_text wants the encoding byte in front */
                        decode_text(b + p - 1, n - p + 1, out, cap);
                        found = out[0] ? 0 : -1;
                    }
                }
                free(b);
            }
            pos = data + size;
        }
    }
    xio_close(fd);
    return found;
}
