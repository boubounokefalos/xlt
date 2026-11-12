/* XLT - code pages and UTF-8. See textenc.h. */
#include <stdlib.h>
#include <string.h>
#include "textenc.h"

const char *enc_name(int enc)
{
    static const char *n[ENC_COUNT] = {"Auto", "UTF-8", "Western (1252)", "Greek (1253)"};
    return enc >= 0 && enc < ENC_COUNT ? n[enc] : "?";
}

/* 0x80..0x9F in Windows-1252 (1253 shares them, minus a few). */
static const uint16_t CP1252_80[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

uint32_t enc_cp(int enc, uint8_t b)
{
    if (b < 0x80)
        return b;
    if (b < 0xA0) {
        uint32_t c = CP1252_80[b - 0x80];
        if (enc == ENC_1253 && (b == 0x88 || b == 0x8A || b == 0x8C || b == 0x8E || b == 0x8F || b == 0x98 ||
                                b == 0x9A || b == 0x9C || b == 0x9E || b == 0x9F))
            return 0;
        return c;
    }
    if (enc != ENC_1253)
        return b; /* Latin-1 */
    switch (b) {
    case 0xA1: return 0x0385;
    case 0xA2: return 0x0386;
    case 0xAA: return 0;
    case 0xAF: return 0x2015;
    case 0xB4: return 0x0384;
    case 0xBB: return 0x00BB;
    case 0xBD: return 0x00BD;
    case 0xD2: return 0;
    case 0xFF: return 0;
    }
    if (b >= 0xB8)
        return b + 0x02D0; /* 0xB8 -> U+0388 ... 0xFE -> U+03CE */
    return b;              /* the rest of 0xA0..0xB7 is as in Latin-1 */
}

int utf8_put(uint32_t cp, char *o)
{
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | cp >> 6);
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp < 0xE000)
        cp = 0xFFFD;
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12);
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp > 0x10FFFF)
        cp = 0xFFFD;
    if (cp < 0x10000)
        return utf8_put(cp, o);
    o[0] = (char)(0xF0 | cp >> 18);
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

uint32_t utf8_get(const uint8_t **sp, const uint8_t *end)
{
    const uint8_t *s = *sp;
    uint32_t c = *s++;
    int more = 0;
    uint32_t min = 0;
    if (c >= 0xF0 && c < 0xF5) { more = 3; c &= 7; min = 0x10000; }
    else if (c >= 0xE0 && c < 0xF0) { more = 2; c &= 15; min = 0x800; }
    else if (c >= 0xC2 && c < 0xE0) { more = 1; c &= 31; min = 0x80; }
    else if (c >= 0x80) { *sp = s; return 0xFFFD; }
    const uint8_t *q = s;
    for (int i = 0; i < more; i++) {
        if (q >= end || (*q & 0xC0) != 0x80) { *sp = s; return 0xFFFD; }
        c = c << 6 | (*q++ & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c < 0xE000)) { *sp = s; return 0xFFFD; }
    *sp = q;
    return c;
}

int utf8_valid(const uint8_t *s, int n)
{
    const uint8_t *end = s + n;
    while (s < end) {
        if (*s < 0x80) { s++; continue; }
        const uint8_t *before = s;
        if (utf8_get(&s, end) == 0xFFFD) {
            /* a character cut off by the end of a sample is fine */
            int need = *before >= 0xF0 ? 4 : *before >= 0xE0 ? 3 : *before >= 0xC2 ? 2 : 0;
            if (need && before + need > end) {
                int ok = 1;
                for (const uint8_t *q = before + 1; q < end; q++)
                    if ((*q & 0xC0) != 0x80) ok = 0;
                if (ok)
                    return 1;
            }
            return 0;
        }
    }
    return 1;
}

static void put_cp(char *out, int *len, uint32_t c)
{
    if (c == '\r')
        return;
    if (c == 0)
        return;
    if ((c < 32 && c != '\n' && c != '\t') || c == 0x7F)
        c = ' ';
    if (c == 0xFEFF)
        return; /* a BOM in the middle */
    *len += utf8_put(c, out + *len);
}

char *text_to_utf8(const uint8_t *s, int n, int enc, int *out_len, int *used)
{
    int u16 = 0; /* 1 = LE, 2 = BE */
    if (n >= 2 && s[0] == 0xFF && s[1] == 0xFE) u16 = 1;
    else if (n >= 2 && s[0] == 0xFE && s[1] == 0xFF) u16 = 2;
    if (enc == ENC_AUTO && !u16)
        enc = utf8_valid(s, n) ? ENC_UTF8 : ENC_1252;
    char *out = malloc((size_t)n * 3 / (u16 ? 2 : 1) + 8);
    if (!out)
        return NULL;
    int len = 0;
    if (u16) {
        for (int i = 2; i + 1 < n; i += 2) {
            uint32_t c = u16 == 1 ? (uint32_t)(s[i] | s[i + 1] << 8) : (uint32_t)(s[i] << 8 | s[i + 1]);
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
                uint32_t d = u16 == 1 ? (uint32_t)(s[i + 2] | s[i + 3] << 8) : (uint32_t)(s[i + 2] << 8 | s[i + 3]);
                if (d >= 0xDC00 && d < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                    i += 2;
                }
            }
            put_cp(out, &len, c);
        }
        if (used) *used = ENC_UTF8;
    } else if (enc == ENC_UTF8) {
        const uint8_t *p = s, *end = s + n;
        if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
            p += 3;
        while (p < end)
            put_cp(out, &len, utf8_get(&p, end));
        if (used) *used = ENC_UTF8;
    } else {
        for (int i = 0; i < n; i++) {
            uint32_t c = enc_cp(enc, s[i]);
            put_cp(out, &len, c ? c : '?');
        }
        if (used) *used = enc;
    }
    out[len] = 0;
    *out_len = len;
    return out;
}

/* Lower case, without Greek accents, final sigma as sigma: what search compares. */
static uint32_t fold(uint32_t c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x391 && c <= 0x3A9) c += 32;
    switch (c) {
    case 0x386: case 0x3AC: return 0x3B1;
    case 0x388: case 0x3AD: return 0x3B5;
    case 0x389: case 0x3AE: return 0x3B7;
    case 0x38A: case 0x3AA: case 0x3CA: case 0x3AF: case 0x390: return 0x3B9;
    case 0x38C: case 0x3CC: return 0x3BF;
    case 0x38E: case 0x3AB: case 0x3CB: case 0x3CD: case 0x3B0: return 0x3C5;
    case 0x38F: case 0x3CE: return 0x3C9;
    case 0x3C2: return 0x3C3;
    }
    return c;
}

int text_find(const char *hay, int len, int from, const char *needle)
{
    uint32_t nd[64];
    int nn = 0;
    const uint8_t *p = (const uint8_t *)needle, *pe = p + strlen(needle);
    while (p < pe && nn < 64)
        nd[nn++] = fold(utf8_get(&p, pe));
    if (!nn)
        return -1;
    const uint8_t *h = (const uint8_t *)hay, *end = h + len;
    if (from < 0) from = 0;
    for (const uint8_t *s = h + from; s < end;) {
        const uint8_t *q = s;
        int k = 0;
        while (k < nn && q < end && fold(utf8_get(&q, end)) == nd[k])
            k++;
        if (k == nn)
            return (int)(s - h);
        utf8_get(&s, end); /* next character */
    }
    return -1;
}

int enc_guess_8bit(const uint8_t *s, int n)
{
    int high = 0, ascii = 0;
    for (int i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c >= 0xC1 && c != 0xD7 && c != 0xF7) high++;
        else if ((c | 32) >= 'a' && (c | 32) <= 'z') ascii++;
    }
    return high > 20 && high * 10 > (high + ascii) * 3 ? ENC_1253 : ENC_1252;
}
