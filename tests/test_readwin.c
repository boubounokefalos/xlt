/* Host test: the read-ahead window returns the file's exact bytes for any
 * pattern of reads, and sequential reads really do hit the file rarely. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/readwin.h"

#define FILE_SIZE 100000u
static uint8_t file[FILE_SIZE];
static int reads;

static int fake_read(void *ctx, uint64_t off, void *dst, uint32_t len)
{
    (void)ctx;
    reads++;
    if (off >= FILE_SIZE) return 0;
    if (off + len > FILE_SIZE) len = FILE_SIZE - (uint32_t)off;
    memcpy(dst, file + off, len);
    return (int)len;
}

static int fails;
#define CHECK(c, m) do { if (c) printf("OK   %s\n", m); else { printf("FAIL %s\n", m); fails++; } } while (0)

int main(void)
{
    static uint8_t win[4096], out[8192];
    for (uint32_t i = 0; i < FILE_SIZE; i++) file[i] = (uint8_t)(i * 2654435761u >> 13);

    ReadWin w;
    readwin_init(&w, win, sizeof(win), fake_read, NULL);
    int bad = 0, sizes = 0;
    uint64_t off = 0;
    reads = 0;
    while (off + 100 <= FILE_SIZE) {
        if (readwin_get(&w, off, 100, out) != 0 || memcmp(out, file + off, 100)) bad++;
        off += 100;
        sizes++;
    }
    CHECK(!bad, "sequential reads return the file's bytes");
    CHECK(reads < sizes / 30, "sequential reads rarely touch the file");

    srand(7);
    bad = 0;
    for (int n = 0; n < 200000; n++) {
        uint32_t len = 1 + (uint32_t)rand() % 8000;
        uint32_t at = (uint32_t)rand() % (FILE_SIZE - len + 1);
        if (readwin_get(&w, at, len, out) != 0 || memcmp(out, file + at, len)) bad++;
    }
    CHECK(!bad, "random reads of any size return the file's bytes");

    CHECK(readwin_get(&w, FILE_SIZE - 10, 10, out) == 0 && !memcmp(out, file + FILE_SIZE - 10, 10),
          "the very end of the file (a short read that still covers it)");
    CHECK(readwin_get(&w, FILE_SIZE - 10, 11, out) != 0, "past the end fails");
    CHECK(readwin_get(&w, FILE_SIZE - 5, 5, out) == 0 && !memcmp(out, file + FILE_SIZE - 5, 5),
          "a failed read leaves nothing stale behind");

    readwin_init(&w, NULL, 4096, fake_read, NULL);
    CHECK(readwin_get(&w, 50, 300, out) == 0 && !memcmp(out, file + 50, 300), "no window: direct reads work");
    printf(fails ? "FAILED\n" : "all good\n");
    return fails != 0;
}
