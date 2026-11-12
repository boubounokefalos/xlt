/* Host test for SETTINGS.TXT parsing and writing (ARK-5 and ARK-4 dialects). */
#include <stdio.h>
#include <string.h>
#include "../src/arkcfg.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("OK   %s\n", msg); else { printf("FAIL %s\n", msg); fails++; } } while (0)

static char extra[4096], out[8192];

int main(void)
{
    ArkCfg c;
    const char *ark5 =
        "always, usbcharge, on\r\n"
        "always, cpuclock:333, on\n"
        "always, highmem:auto, on\n"
        "always, mscache:8k, on\n"
        "always, infernocache:rr, on\n"
        "game, noled, on\n"
        "always, skiplogos:gameboot, on\n"
        "vsh, region_us, on\n"
        "# my comment\n"
        "ULUS10041, cpuclock:222, on\n"
        "always, someunknownkey, on\n"
        "\n";
    arkcfg_parse(&c, ark5, 0, extra, sizeof(extra));
    CHECK(c.value[AK_USBCHARGE] == 1, "bool on");
    CHECK(c.value[AK_CLOCK_GAME] == ACLK_333 && c.value[AK_CLOCK_VSH] == ACLK_333, "always clock sets both");
    CHECK(c.value[AK_HIGHMEM] == 2 && c.value[AK_MSCACHE] == 2 && c.value[AK_INFERNOCACHE] == 2, "options");
    CHECK(c.value[AK_NOLED] == 1 && c.level[AK_NOLED] == RL_GAME, "runlevel kept");
    CHECK(c.value[AK_SKIPLOGOS] == 2 && c.value[AK_REGION] == 2, "skiplogos and region");
    CHECK(c.value[AK_WPA2] == 0, "missing key is off (file exists)");
    CHECK(strstr(extra, "# my comment\n") && strstr(extra, "ULUS10041, cpuclock:222, on\n") &&
          strstr(extra, "always, someunknownkey, on\n"), "unknown, comment and per-game lines kept");

    int n = arkcfg_write(&c, 0, extra, out, sizeof(out));
    CHECK(n > 0 && strstr(out, "game, noled, on\n"), "writes back on its runlevel");
    CHECK(strstr(out, "always, cpuclock:333, on\n") != NULL, "equal clocks as one always line");
    CHECK(strstr(out, "ULUS10041, cpuclock:222, on\n") != NULL, "per-game line survives a write");

    ArkCfg c2;
    static char extra2[4096];
    arkcfg_parse(&c2, out, 0, extra2, sizeof(extra2));
    CHECK(!memcmp(c.value, c2.value, sizeof(c.value)), "round trip: same values");
    CHECK(!strcmp(extra, extra2), "round trip: same extra lines");

    arkcfg_set(&c, AK_CLOCK_GAME, ACLK_222);
    arkcfg_write(&c, 0, extra, out, sizeof(out));
    CHECK(strstr(out, "game, cpuclock:222, on\n") && strstr(out, "vsh, cpuclock:333, on\n") &&
          !strstr(out, "always, cpuclock"), "game clock changed: XMB keeps its clock");

    arkcfg_set(&c, AK_WPA2, 1);
    arkcfg_write(&c, 0, extra, out, sizeof(out));
    CHECK(strstr(out, "always, wpa2, on\n") != NULL, "switched on: always");

    arkcfg_set(&c, AK_CLOCK_GAME, ACLK_AUTO);
    arkcfg_write(&c, 0, extra, out, sizeof(out));
    CHECK(!strstr(out, "game, cpuclock") && strstr(out, "vsh, cpuclock:333, on\n"), "auto: no game clock line");

    /* ARK-4 dialect */
    const char *ark4 = "always, overclock, on\nalways, highmem, on\nalways, mscache, on\n";
    arkcfg_parse(&c, ark4, 1, extra, sizeof(extra));
    CHECK(c.value[AK_CLOCK_GAME] == ACLK_333 && c.value[AK_HIGHMEM] == 1 && c.value[AK_MSCACHE] == 1, "ARK-4 read");
    arkcfg_set(&c, AK_CLOCK_GAME, ACLK_133);
    arkcfg_write(&c, 1, extra, out, sizeof(out));
    CHECK(strstr(out, "game, powersave, on\n") && strstr(out, "vsh, overclock, on\n"), "ARK-4 clock keys");
    CHECK(strstr(out, "always, highmem, on\n") && !strstr(out, "deadef") && !strstr(out, "vitamute"),
          "ARK-4 on/off keys, no ARK-5 keys");

    arkcfg_defaults(&c, 0);
    CHECK(c.value[AK_USBCHARGE] == 1 && c.value[AK_CLOCK_GAME] == ACLK_333 && c.value[AK_HIGHMEM] == 2,
          "defaults as arkMenu");
    CHECK(arkcfg_write(&c, 0, "", out, 64) == -1, "too small buffer reports -1");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
