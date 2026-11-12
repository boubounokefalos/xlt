/* sfokey.c: one key out of a PARAM.SFO, made here the way the PSP writes them. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sfokey.h"

static uint8_t sfo[1024];
static int nkeys;
static uint32_t koff, doff;

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* A PARAM.SFO with up to 8 keys: entries, then the key table, then the data. */
static void add(const char *key, int fmt, const void *val, int len, int room)
{
    uint8_t *e = sfo + 20 + nkeys * 16;
    e[0] = koff; e[1] = koff >> 8;
    e[2] = fmt; e[3] = fmt >> 8;
    put32(e + 4, len);
    put32(e + 8, room);
    put32(e + 12, doff);
    strcpy((char *)sfo + 20 + 8 * 16 + koff, key);
    koff += strlen(key) + 1;
    memcpy(sfo + 512 + doff, val, len);
    doff += room;
    nkeys++;
}

int main(void)
{
    memcpy(sfo, "\0PSF", 4);
    put32(sfo + 4, 0x101);
    put32(sfo + 8, 20 + 8 * 16);
    put32(sfo + 12, 512);
    uint32_t parental = 5;
    add("CATEGORY", 0x0204, "MS", 3, 4);
    add("PARENTAL_LEVEL", 0x0404, &parental, 4, 4);
    add("SAVEDATA_DETAIL", 0x0204, "Chapter 3\nThe Forest", 21, 128);
    add("SAVEDATA_DIRECTORY", 0x0204, "ULUS10041DATA00", 16, 64);
    add("TITLE", 0x0204, "\xCE\x9A\xCE\xB1\xCE\xBB\xCF\x8C Game", 14, 128);
    put32(sfo + 16, nkeys);
    char v[64];
    assert(sfo_get(sfo, sizeof(sfo), "CATEGORY", v, sizeof(v)) == 0 && !strcmp(v, "MS"));
    assert(sfo_get(sfo, sizeof(sfo), "PARENTAL_LEVEL", v, sizeof(v)) == 0 && !strcmp(v, "5"));
    assert(sfo_get(sfo, sizeof(sfo), "SAVEDATA_DETAIL", v, sizeof(v)) == 0 && !strcmp(v, "Chapter 3 The Forest"));
    assert(sfo_get(sfo, sizeof(sfo), "SAVEDATA_DIRECTORY", v, sizeof(v)) == 0 && !strcmp(v, "ULUS10041DATA00"));
    assert(sfo_get(sfo, sizeof(sfo), "TITLE", v, sizeof(v)) == 0 && !strcmp(v, "\xCE\x9A\xCE\xB1\xCE\xBB\xCF\x8C Game"));
    assert(sfo_get(sfo, sizeof(sfo), "TITL", v, sizeof(v)) == -1 && !v[0]); /* a prefix isn't the key */
    assert(sfo_get(sfo, sizeof(sfo), "SAVEDATA_DIRECTORY", v, 8) == 0 && !strcmp(v, "ULUS100")); /* cut to fit */
    assert(sfo_get(sfo, 40, "TITLE", v, sizeof(v)) == -1);      /* cut short: refused, nothing read past it */
    uint8_t bad[32] = "\0PSX";
    assert(sfo_get(bad, sizeof(bad), "TITLE", v, sizeof(v)) == -1);
    put32(sfo + 16, 100000);                                     /* a silly entry count */
    assert(sfo_get(sfo, sizeof(sfo), "TITLE", v, sizeof(v)) == -1);
    puts("test_sfokey: ok");
    return 0;
}
