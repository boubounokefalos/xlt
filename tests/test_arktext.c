/* Host test for SETTINGS.TXT clock editing. */
#include <stdio.h>
#include <string.h>
#include "../src/ark.h"
#include "../src/arktext.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("OK   %s\n", msg); else { printf("FAIL %s\n", msg); fails++; } } while (0)

static const char *SAMPLE =
    "always, usbcharge, on\r\n"
    "always, cpuclock:333, on\r\n"
    "# comment, with, commas\r\n"
    "\r\n"
    "ULUS10041, cpuclock:222, on\r\n"
    "ULUS10041, highmem, on\r\n"
    "NPUH10001, overclock, on\r\n";

int main(void)
{
    char out[4096], out2[4096];

    CHECK(arktext_get_clock(SAMPLE, NULL) == CLOCK_333, "global game clock read from 'always'");
    CHECK(arktext_get_clock(SAMPLE, "ULUS10041") == CLOCK_222, "per-game clock read");
    CHECK(arktext_get_clock(SAMPLE, "NPUH10001") == CLOCK_333, "ARK-4 style key read");
    CHECK(arktext_get_clock(SAMPLE, "ULES00000") == CLOCK_AUTO, "unknown game is Auto");

    int n = arktext_set_clock(SAMPLE, "ULUS10041", CLOCK_333, 0, out, sizeof(out));
    out[n > 0 ? n : 0] = 0;
    CHECK(n > 0 && arktext_get_clock(out, "ULUS10041") == CLOCK_333, "per-game set to 333");
    CHECK(strstr(out, "ULUS10041, highmem, on\n") != NULL, "other per-game line kept");
    CHECK(strstr(out, "# comment, with, commas\n") != NULL, "comment kept");
    CHECK(strstr(out, "always, cpuclock:333, on\n") != NULL, "global line untouched by per-game edit");
    CHECK(strstr(out, "ULUS10041, cpuclock:222") == NULL, "old per-game clock removed");

    n = arktext_set_clock(out, "ULUS10041", CLOCK_AUTO, 0, out2, sizeof(out2));
    out2[n > 0 ? n : 0] = 0;
    CHECK(n > 0 && strstr(out2, "ULUS10041, cpuclock") == NULL, "Auto removes the per-game line");
    CHECK(arktext_get_clock(out2, "ULUS10041") == CLOCK_AUTO, "per-game now Auto");

    n = arktext_set_clock(SAMPLE, NULL, CLOCK_222, 0, out, sizeof(out));
    out[n > 0 ? n : 0] = 0;
    CHECK(strstr(out, "vsh, cpuclock:333, on\n") != NULL, "'always' clock kept for the XMB only");
    CHECK(strstr(out, "game, cpuclock:222, on\n") != NULL, "game default line added");
    CHECK(arktext_get_clock(out, NULL) == CLOCK_222, "global game clock now 222");
    CHECK(strstr(out, "always, usbcharge, on\n") != NULL, "non-clock always line kept");

    n = arktext_set_clock("", "ULJM05500", CLOCK_133, 1, out, sizeof(out));
    out[n > 0 ? n : 0] = 0;
    CHECK(!strcmp(out, "ULJM05500, powersave, on\n"), "ARK-4 key written for empty file");

    CHECK(arktext_set_clock(SAMPLE, NULL, CLOCK_222, 0, out, 20) == -1, "too small buffer reports error");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
