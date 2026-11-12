/* Host test for plugin/xltshot/pngout.c, the in-game screenshot plugin's PNG
 * writer: a 480x272 picture made of gradients, flat areas and noise is written
 * row by row with only PNGOUT_MEM bytes of memory, then read back by libpng and
 * compared pixel for pixel; a failing writer must be reported.
 *
 *   test_pngout OUT.png */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>
#include "pngout.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); puts(""); fails++; } } while (0)

static int to_file(void *ctx, const void *d, int n) { return fwrite(d, 1, n, ctx) == (size_t)n ? 0 : -1; }
static int broken_after;
static int to_nowhere(void *ctx, const void *d, int n) { (void)ctx; (void)d; return (broken_after -= n) < 0 ? -1 : 0; }

static unsigned char pic[272][480 * 3];

static void make(void)
{
    unsigned s = 12345;
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++) {
            unsigned char *p = &pic[y][x * 3];
            if (y < 90) { p[0] = x * 255 / 479; p[1] = y * 255 / 89; p[2] = 128; }          /* gradients */
            else if (y < 180) { p[0] = p[1] = p[2] = (x / 40) & 1 ? 0x0d : 0xe6; }         /* flat */
            else { s = s * 1103515245 + 12345; p[0] = s >> 16; p[1] = s >> 8; p[2] = s >> 24; } /* noise */
        }
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    make();
    static unsigned char mem[PNGOUT_MEM];
    FILE *f = fopen(argv[1], "wb");
    CHECK(f, "can't write %s", argv[1]);
    if (!f) return 1;
    PngOut p;
    int r = pngout_begin(&p, mem, 480, 272, to_file, f);
    for (int y = 0; r == 0 && y < 272; y++) r = pngout_row(&p, pic[y]);
    if (r == 0) r = pngout_end(&p);
    fclose(f);
    CHECK(r == 0, "writing failed (%d)", r);
    CHECK(p.used <= PNGOUT_MEM, "used %u of %u bytes", p.used, PNGOUT_MEM);
    printf("test_pngout: zlib and the buffer took %u of %u bytes\n", p.used, PNGOUT_MEM);

    /* read back */
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    CHECK(png_image_begin_read_from_file(&img, argv[1]), "libpng can't open it: %s", img.message);
    img.format = PNG_FORMAT_RGB;
    static unsigned char back[272][480 * 3];
    CHECK(img.width == 480 && img.height == 272, "size %ux%u", img.width, img.height);
    CHECK(png_image_finish_read(&img, NULL, back, 480 * 3, NULL), "libpng can't read it: %s", img.message);
    CHECK(!memcmp(back, pic, sizeof(pic)), "pixels differ");

    /* a write that fails partway: an error, not a crash */
    broken_after = 20000;
    r = pngout_begin(&p, mem, 480, 272, to_nowhere, NULL);
    for (int y = 0; r == 0 && y < 272; y++) r = pngout_row(&p, pic[y]);
    if (r == 0) r = pngout_end(&p);
    CHECK(r != 0, "a failing write wasn't reported");
    /* too few rows */
    r = pngout_begin(&p, mem, 480, 272, to_nowhere, NULL);
    broken_after = 1 << 30;
    CHECK(pngout_end(&p) != 0, "ending early wasn't refused");

    puts(fails ? "test_pngout: FAILED" : "test_pngout: ok");
    return fails != 0;
}
