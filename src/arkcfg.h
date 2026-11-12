/* XLT - ARK's CFW settings (SETTINGS.TXT) as a struct. Portable (host-tested).
 *
 * Read and written in the format of ARK's own arkMenu (and psp5, tested on
 * hardware), in ARK-5's or ARK-4's dialect. Every setting keeps the runlevel
 * it was found on ("always", "game", ...); one switched on from off gets
 * "always". Lines that aren't one of these settings (comments, per-game lines
 * such as "ULUS10041, cpuclock:333, on", unknown keys) are kept as they are. */
#ifndef ARKCFG_H
#define ARKCFG_H

/* Runlevels, as arkMenu names them. 0 = off. */
enum { RL_OFF, RL_ALWAYS, RL_GAME, RL_UMD, RL_HOMEBREW, RL_POPS, RL_VSH, RL_LAUNCHER };

/* CPU speeds. ARK-4 only knows the first four. */
enum { ACLK_AUTO, ACLK_133, ACLK_222, ACLK_333, ACLK_383, ACLK_403, ACLK_423, ACLK_443, ACLK_COUNT };

enum {
    AK_USBCHARGE, AK_WPA2, AK_LAUNCHER, AK_DISABLEPAUSE, AK_OLDPLUGIN, AK_HIBBLOCK,
    AK_HIDEMAC, AK_HIDEDLC, AK_NOLED, AK_NOUMD, AK_DEADEF, AK_NOANALOG, AK_VITAMUTE, AK_QAFLAGS,
    AK_HIGHMEM,      /* 0 off, 1 on, 2 auto, 3 +16 MiB, 4 max (ARK-4: 0/1) */
    AK_MSCACHE,      /* 0 off, 1 4 KiB, 2 8 KiB, 3 16 KiB (ARK-4: 0/1) */
    AK_INFERNOCACHE, /* 0 off, 1 LRU, 2 RR */
    AK_SKIPLOGOS,    /* 0 off, 1 all, 2 game boot, 3 cold boot */
    AK_HIDEPICS,     /* 0 off, 1 all, 2 PIC0, 3 PIC1 */
    AK_CLOCK_GAME,   /* ACLK_* */
    AK_CLOCK_VSH,
    AK_REGION,       /* UMD region: 0 default, 1 JP, 2 US, 3 EU */
    AK_FAKEREGION,   /* XMB region: 0 default, else fakeregion_N */
    AK_COUNT
};

typedef struct {
    int value[AK_COUNT];
    int level[AK_COUNT]; /* runlevel it is written on, RL_* (never RL_OFF) */
} ArkCfg;

/* ARK's defaults when there is no SETTINGS.TXT (arkMenu's resetCfwSettings). */
void arkcfg_defaults(ArkCfg *c, int ark4);

/* Parses text into c. Lines that aren't settings are copied, one per line,
 * into extra (cap bytes, always terminated). */
void arkcfg_parse(ArkCfg *c, const char *text, int ark4, char *extra, int cap);

/* Writes the whole file: settings first, a blank line, then extra.
 * Returns the length, or -1 if it doesn't fit. */
int arkcfg_write(const ArkCfg *c, int ark4, const char *extra, char *out, int cap);

/* Sets a value. Switching a setting on from off puts it on "always". */
void arkcfg_set(ArkCfg *c, int key, int value);

#endif
