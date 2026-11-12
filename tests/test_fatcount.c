/* Host test for src/fatcount.c over the images tests/make_fat.py writes:
 * the geometry and the free count must agree with fsck.fat, read in uneven
 * chunks the way the PSP thread reads them.
 *
 *   test_fatcount DIR   (DIR/fat.expect lists the images) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fatcount.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); puts(""); fails++; } } while (0)

static unsigned char *slurp(const char *p, long *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n);
    if (fread(b, 1, *n, f) != (size_t)*n) { free(b); b = NULL; }
    fclose(f);
    return b;
}

static void one(const char *dir, const char *name, unsigned total, unsigned want_free, unsigned csize)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s.img", dir, name);
    long n;
    unsigned char *d = slurp(p, &n);
    CHECK(d, "%s: can't read", p);
    if (!d) return;
    long part = 0;
    int64_t lba = fat_mbr_first(d);
    if (lba >= 0) part = (long)lba * 512;
    CHECK((lba >= 0) == (strstr(name, "mbr") != NULL), "%s: MBR seen %d", name, lba >= 0);
    FatGeom g;
    CHECK(fat_parse_boot(d + part, &g) == 0, "%s: boot sector refused", name);
    CHECK(g.clusters == total, "%s: %u clusters, fsck says %u", name, g.clusters, total);
    CHECK(g.cluster == csize, "%s: cluster %u, want %u", name, g.cluster, csize);
    /* in chunks of an odd size that is a multiple of 4 (an entry never splits) */
    uint32_t free_n = 0, step = 4 * 1237;
    for (uint32_t at = 0; at < g.fat_bytes; at += step) {
        uint32_t len = g.fat_bytes - at < step ? g.fat_bytes - at : step;
        free_n += fat_count_free(&g, d + part + g.fat_offset + at, len, at);
    }
    CHECK(free_n == want_free, "%s: %u free, want %u", name, free_n, want_free);
    /* in one go too */
    CHECK(fat_count_free(&g, d + part + g.fat_offset, g.fat_bytes, 0) == want_free, "%s: one-go count", name);
    printf("%s: FAT%d, %u clusters of %u, %u free\n", name, g.type, g.clusters, g.cluster, free_n);
    free(d);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    char p[512];
    snprintf(p, sizeof(p), "%s/fat.expect", argv[1]);
    FILE *f = fopen(p, "r");
    CHECK(f, "no %s", p);
    char name[64];
    unsigned total, want, csize;
    int seen = 0;
    while (f && fscanf(f, "%63s %u %u %u", name, &total, &want, &csize) == 4) {
        one(argv[1], name, total, want, csize);
        seen++;
    }
    if (f) fclose(f);
    CHECK(seen == 3, "%d images", seen);
    /* not FAT: zeros, and a boot sector with a nonsense sector size */
    unsigned char z[512] = {0};
    FatGeom g;
    CHECK(fat_parse_boot(z, &g) < 0 && fat_mbr_first(z) < 0, "zeros taken for FAT");
    z[510] = 0x55; z[511] = 0xAA; z[11] = 0x10; z[12] = 0x01; z[13] = 1;
    CHECK(fat_parse_boot(z, &g) < 0, "sector size 272 taken");
    puts(fails ? "test_fatcount: FAILED" : "test_fatcount: ok");
    return fails != 0;
}
