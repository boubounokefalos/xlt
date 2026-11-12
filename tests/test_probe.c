/* Host test: probe each file and compare its ICON0 texture with the expected bytes.
 * Usage: test_probe FILE EXPECT_TITLE EXPECT_KIND ICON_EXPECT|- [...] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/entry.h"
#include "../src/thumbconv.h"

static int check(const char *path, const char *title, int kind, const char *expect)
{
    Entry e;
    memset(&e, 0, sizeof(e));
    snprintf(e.path, sizeof(e.path), "%s", path);
    int r = probe_entry(&e);
    if (kind < 0) {
        printf("%-28s %s (rejected as expected)\n", path, r ? "OK  " : "FAIL");
        return r ? 0 : 1;
    }
    int ok = r == 0 && !strcmp(e.title, title) && e.kind == kind;
    printf("%-28s %s title='%s' id=%s fw=%s kind=%d icon=%u@%u\n", path, ok ? "OK  " : "FAIL",
           e.title, e.id, e.sysver, e.kind, e.icon_size, e.icon_off);
    if (!ok)
        return 1;
    if (strcmp(expect, "-")) {
        static uint8_t got[THUMB_BYTES], want[THUMB_BYTES];
        FILE *f = fopen(expect, "rb");
        if (!f || fread(want, 1, THUMB_BYTES, f) != THUMB_BYTES) return 1;
        fclose(f);
        int t = thumb_from_entry(e.path, kind_is_disc(e.kind), e.icon_off, e.icon_size, got);
        int same = t == 0 && !memcmp(got, want, THUMB_BYTES);
        printf("%-28s %s thumbnail matches expected swizzled 5551\n", "", same ? "OK  " : "FAIL");
        if (!same) return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int fails = 0;
    for (int i = 1; i + 3 < argc; i += 4)
        fails += check(argv[i], argv[i + 1], atoi(argv[i + 2]), argv[i + 3]);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
