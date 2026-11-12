/* XLT - one PARAM.SFO key. See sfokey.h.
 *
 * The layout: "\0PSF", version, then the offsets of the key table and the data
 * table and the entry count; 16-byte entries follow from byte 20 (key offset
 * u16, format u16, length u32, room u32, data offset u32). Format 0x0204 is a
 * UTF-8 string, 0x0404 a 32-bit number. Everything is checked against n. */
#include <stdio.h>
#include <string.h>
#include "sfokey.h"

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

int sfo_get(const uint8_t *b, uint32_t n, const char *key, char *out, int cap)
{
    if (cap > 0) out[0] = 0;
    if (n < 20 || memcmp(b, "\0PSF", 4))
        return -1;
    uint32_t keys = le32(b + 8), data = le32(b + 12), count = le32(b + 16);
    if (keys > n || data > n || count > (n - 20) / 16)
        return -1;
    size_t kl = strlen(key);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = b + 20 + i * 16;
        uint32_t ko = keys + le16(e), fmt = le16(e + 2), len = le32(e + 4), off = data + le32(e + 12);
        if (ko + kl + 1 > n || memcmp(b + ko, key, kl + 1))
            continue;
        if (off > n || len > n - off)
            return -1;
        if (fmt == 0x0404 && len >= 4) {
            snprintf(out, cap, "%u", (unsigned)le32(b + off));
            return 0;
        }
        uint32_t m = 0;
        while (m < len && b[off + m]) m++; /* the text stops at its NUL */
        if (m >= (uint32_t)cap) m = cap - 1;
        memcpy(out, b + off, m);
        out[m] = 0;
        /* line breaks in a save's details: spaces, for one-line display */
        for (uint32_t k = 0; k < m; k++)
            if (out[k] == '\n' || out[k] == '\r') out[k] = ' ';
        return 0;
    }
    return -1;
}
