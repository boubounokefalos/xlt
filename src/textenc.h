/* XLT - 8-bit code pages and UTF-8 helpers for the reader and the PDF text
 * extractor. Portable (host-tested). */
#ifndef TEXTENC_H
#define TEXTENC_H

#include <stdint.h>

enum {
    ENC_AUTO,   /* reader only: UTF-8 if the bytes are valid UTF-8, else Windows-1252 */
    ENC_UTF8,
    ENC_1252,   /* Western (Windows-1252, a superset of Latin-1's printable range) */
    ENC_1253,   /* Greek (Windows-1253; ISO-8859-7 differs in two letters only) */
    ENC_COUNT
};

const char *enc_name(int enc);

/* The code point of byte b in a single-byte code page (ENC_1252 / ENC_1253),
 * 0 if the byte is undefined there. Bytes below 0x80 are ASCII. */
uint32_t enc_cp(int enc, uint8_t b);

/* Writes cp as UTF-8 into out (room for 4 bytes); returns the byte count. */
int utf8_put(uint32_t cp, char *out);

/* Decodes one UTF-8 character at *s (not past end) and advances *s.
 * Malformed bytes come back as 0xFFFD, one byte at a time. */
uint32_t utf8_get(const uint8_t **s, const uint8_t *end);

/* 1 if the n bytes are valid UTF-8 (a character cut by the end still counts). */
int utf8_valid(const uint8_t *s, int n);

/* Converts n bytes in enc (or UTF-16 with a BOM, detected here) to UTF-8.
 * Returns a malloc'd, NUL-terminated buffer and its length in *out_len;
 * carriage returns are dropped and other control characters but \n and \t
 * become spaces. *used gets the encoding that was applied. */
char *text_to_utf8(const uint8_t *s, int n, int enc, int *out_len, int *used);

/* An 8-bit text that isn't UTF-8: ENC_1253 (Greek) if most of its letters are
 * above ASCII, as Greek text is, else ENC_1252 (Western text has an accented
 * letter here and there). For subtitle and lyrics files. */
int enc_guess_8bit(const uint8_t *s, int n);

/* Byte offset of the first match of needle in hay at or after byte `from`,
 * ignoring case and Greek accents; -1 if none. */
int text_find(const char *hay, int len, int from, const char *needle);

#endif
