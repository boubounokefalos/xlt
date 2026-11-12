/* XLT - SETTINGS.TXT <-> ArkCfg. See arkcfg.h. Logic as arkMenu / psp5's arkcfg.c. */
/* Parts of this file (the SETTINGS.TXT parser) are adapted from psp5 by 08Weber,
 * https://github.com/08Weber/psp5, GPL-3.0. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "arkcfg.h"

#define LINE_LEN 160

static const char *const CLOCK_KEYS[ACLK_COUNT] = {"", "133", "222", "333", "383", "403", "423", "443"};
static const char *const RL_NAMES[] = {"", "always", "game", "umd", "homebrew", "pops", "vsh", "launcher"};

static const struct { const char *name; int key; } BOOLS[] = {
    {"usbcharge", AK_USBCHARGE}, {"wpa2", AK_WPA2}, {"launcher", AK_LAUNCHER},
    {"disablepause", AK_DISABLEPAUSE}, {"oldplugin", AK_OLDPLUGIN}, {"hibblock", AK_HIBBLOCK},
    {"hidemac", AK_HIDEMAC}, {"hidedlc", AK_HIDEDLC}, {"noled", AK_NOLED}, {"noumd", AK_NOUMD},
    {"deadef", AK_DEADEF}, {"noanalog", AK_NOANALOG}, {"vitamute", AK_VITAMUTE}, {"vitmute", AK_VITAMUTE},
    {"qaflags", AK_QAFLAGS},
};

void arkcfg_defaults(ArkCfg *c, int ark4)
{
    memset(c, 0, sizeof(*c));
    for (int k = 0; k < AK_COUNT; k++)
        c->level[k] = RL_ALWAYS;
    c->level[AK_REGION] = c->level[AK_FAKEREGION] = RL_VSH;
    c->value[AK_USBCHARGE] = 1;
    c->value[AK_CLOCK_GAME] = c->value[AK_CLOCK_VSH] = ACLK_333;
    c->value[AK_HIGHMEM] = ark4 ? 0 : 2; /* ARK-4: "highmem, on" forces extra memory */
    c->value[AK_WPA2] = 1;
    c->value[AK_MSCACHE] = 1;
    c->value[AK_INFERNOCACHE] = 1;
    c->value[AK_OLDPLUGIN] = 1;
    c->value[AK_HIBBLOCK] = 1;
    c->value[AK_HIDEMAC] = 1;
    c->value[AK_HIDEDLC] = 1;
    c->value[AK_VITAMUTE] = 1;
    c->value[AK_QAFLAGS] = 1;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

static int runlevel_of(const char *rl)
{
    if (!strcasecmp(rl, "always") || !strcasecmp(rl, "all")) return RL_ALWAYS;
    if (!strcasecmp(rl, "game")) return RL_GAME;
    if (!strcasecmp(rl, "umd") || !strcasecmp(rl, "psp")) return RL_UMD;
    if (!strcasecmp(rl, "homebrew")) return RL_HOMEBREW;
    if (!strcasecmp(rl, "pops") || !strcasecmp(rl, "psx") || !strcasecmp(rl, "ps1")) return RL_POPS;
    if (!strcasecmp(rl, "vsh") || !strcasecmp(rl, "xmb")) return RL_VSH;
    if (!strcasecmp(rl, "launcher")) return RL_LAUNCHER;
    return -1; /* a game ID: kept as its own line */
}

static int is_on(const char *v)
{
    return !strcasecmp(v, "on") || !strcmp(v, "1") || !strcasecmp(v, "enabled") || !strcasecmp(v, "true");
}

static void put_val(ArkCfg *c, int key, int value, int level, int on)
{
    if (!on) {
        c->value[key] = 0;
        return;
    }
    c->value[key] = value;
    c->level[key] = level;
}

static void set_clock(ArkCfg *c, int level, int value)
{
    if (level == RL_VSH) {
        c->value[AK_CLOCK_VSH] = value;
    } else if (level == RL_ALWAYS) {
        c->value[AK_CLOCK_GAME] = c->value[AK_CLOCK_VSH] = value;
    } else {
        c->value[AK_CLOCK_GAME] = value;
        c->level[AK_CLOCK_GAME] = level;
    }
}

/* Returns 1 if the line was a setting. */
static int parse_setting(ArkCfg *c, const char *rl, const char *key, const char *en)
{
    int level = runlevel_of(rl);
    if (level < 0)
        return 0;
    int on = is_on(en);
    const char *opt = strchr(key, ':');
    opt = opt ? opt + 1 : "";
    for (unsigned i = 0; i < sizeof(BOOLS) / sizeof(BOOLS[0]); i++)
        if (!strcasecmp(key, BOOLS[i].name)) {
            put_val(c, BOOLS[i].key, 1, level, on);
            return 1;
        }
    if (!strncasecmp(key, "cpuclock", 8)) {
        if (on)
            for (int k = 1; k < ACLK_COUNT; k++)
                if (!strcasecmp(opt, CLOCK_KEYS[k]))
                    set_clock(c, level, k);
    } else if (!strcasecmp(key, "overclock")) {
        if (on) set_clock(c, level, ACLK_333);
    } else if (!strcasecmp(key, "powersave")) {
        if (on) set_clock(c, level, ACLK_133);
    } else if (!strcasecmp(key, "defaultclock")) {
        if (on) set_clock(c, level, ACLK_222);
    } else if (!strncasecmp(key, "highmem", 7)) {
        put_val(c, AK_HIGHMEM, !strcasecmp(opt, "auto") ? 2 : !strcasecmp(opt, "force16") ? 3
                             : !strcasecmp(opt, "forcemax") ? 4 : 1, level, on);
    } else if (!strncasecmp(key, "mscache", 7)) {
        put_val(c, AK_MSCACHE, !strcasecmp(opt, "8k") ? 2 : !strcasecmp(opt, "16k") ? 3 : 1, level, on);
    } else if (!strncasecmp(key, "infernocache", 12)) {
        put_val(c, AK_INFERNOCACHE, !strcasecmp(opt, "rr") ? 2 : 1, level, on);
    } else if (!strncasecmp(key, "skiplogos", 9)) {
        put_val(c, AK_SKIPLOGOS, !strcasecmp(opt, "gameboot") ? 2 : !strcasecmp(opt, "coldboot") ? 3 : 1, level, on);
    } else if (!strncasecmp(key, "hidepics", 8)) {
        put_val(c, AK_HIDEPICS, !strcasecmp(opt, "pic0") ? 2 : !strcasecmp(opt, "pic1") ? 3 : 1, level, on);
    } else if (!strncasecmp(key, "region_", 7)) {
        int r = !strcasecmp(key + 7, "jp") ? 1 : !strcasecmp(key + 7, "us") ? 2 : !strcasecmp(key + 7, "eu") ? 3 : 0;
        if (on && r) put_val(c, AK_REGION, r, level, 1);
    } else if (!strncasecmp(key, "fakeregion_", 11)) {
        if (on) put_val(c, AK_FAKEREGION, atoi(key + 11), level, 1);
    } else {
        return 0;
    }
    return 1;
}

static void append(char *buf, int cap, int *len, const char *s)
{
    int n = (int)strlen(s);
    if (*len < 0 || *len + n >= cap) {
        *len = -1;
        return;
    }
    memcpy(buf + *len, s, n + 1);
    *len += n;
}

void arkcfg_parse(ArkCfg *c, const char *text, int ark4, char *extra, int cap)
{
    arkcfg_defaults(c, ark4);
    /* the file exists: start from nothing, as arkMenu does, not from the defaults */
    memset(c->value, 0, sizeof(c->value));
    int elen = 0;
    if (cap > 0)
        extra[0] = 0;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        char raw[LINE_LEN];
        snprintf(raw, sizeof(raw), "%.*s", n < LINE_LEN - 1 ? n : LINE_LEN - 1, p);
        p += n + (nl ? 1 : 0);
        char *t = trim(raw);
        if (!*t)
            continue; /* blank lines are rewritten by arkcfg_write */
        char tmp[LINE_LEN], *parts[4] = {0}, *save = NULL;
        int k = 0;
        snprintf(tmp, sizeof(tmp), "%s", t);
        for (char *q = strtok_r(tmp, ",", &save); q && k < 4; q = strtok_r(NULL, ",", &save))
            parts[k++] = trim(q);
        int comment = t[0] == '#' || t[0] == ';' || (t[0] == '/' && t[1] == '/');
        if (comment || k != 3 || !parse_setting(c, parts[0], parts[1], parts[2])) {
            int before = elen;
            append(extra, cap, &elen, t);
            append(extra, cap, &elen, "\n");
            if (elen < 0) { /* full: keep what fitted */
                elen = before;
                if (cap > 0) extra[before] = 0;
            }
        }
    }
}

static void put_line(char *out, int cap, int *len, int level, const char *key, int on)
{
    char line[LINE_LEN];
    snprintf(line, sizeof(line), "%s, %s, %s\n", RL_NAMES[level > 0 && level <= RL_LAUNCHER ? level : RL_ALWAYS],
             key, on ? "on" : "off");
    append(out, cap, len, line);
}

static void put_clock(char *out, int cap, int *len, int ark4, const char *rl, int clock)
{
    if (clock <= ACLK_AUTO || clock >= ACLK_COUNT)
        return; /* Auto: no line */
    char line[LINE_LEN];
    if (ark4) {
        if (clock > ACLK_333) clock = ACLK_333;
        const char *key = clock == ACLK_133 ? "powersave" : clock == ACLK_222 ? "defaultclock" : "overclock";
        snprintf(line, sizeof(line), "%s, %s, on\n", rl, key);
    } else {
        snprintf(line, sizeof(line), "%s, cpuclock:%s, on\n", rl, CLOCK_KEYS[clock]);
    }
    append(out, cap, len, line);
}

int arkcfg_write(const ArkCfg *c, int ark4, const char *extra, char *out, int cap)
{
    int len = 0;
    if (cap > 0) out[0] = 0;
    const int *v = c->value, *l = c->level;
#define B(key, name) put_line(out, cap, &len, l[key], name, v[key] != 0)
    B(AK_USBCHARGE, "usbcharge");
    int cg = v[AK_CLOCK_GAME], cv = v[AK_CLOCK_VSH];
    if (cg == cv && l[AK_CLOCK_GAME] == RL_ALWAYS) {
        put_clock(out, cap, &len, ark4, "always", cg);
    } else {
        int gl = l[AK_CLOCK_GAME];
        /* "always" would also cover the XMB: games get "game" (or their own runlevel) */
        int gr = gl > RL_ALWAYS && gl <= RL_LAUNCHER && gl != RL_VSH ? gl : RL_GAME;
        put_clock(out, cap, &len, ark4, RL_NAMES[gr], cg);
        put_clock(out, cap, &len, ark4, "vsh", cv);
    }
    B(AK_WPA2, "wpa2");
    B(AK_LAUNCHER, "launcher");
    static const char *const highmem[] = {"highmem", "highmem", "highmem:auto", "highmem:force16", "highmem:forcemax"};
    static const char *const mscache[] = {"mscache", "mscache:4k", "mscache:8k", "mscache:16k"};
    static const char *const inferno[] = {"infernocache", "infernocache:lru", "infernocache:rr"};
    static const char *const logos[] = {"skiplogos", "skiplogos", "skiplogos:gameboot", "skiplogos:coldboot"};
    static const char *const pics[] = {"hidepics", "hidepics", "hidepics:pic0", "hidepics:pic1"};
    if (ark4) { /* ARK-4: on/off only */
        B(AK_HIGHMEM, "highmem");
        B(AK_MSCACHE, "mscache");
    } else {
        put_line(out, cap, &len, l[AK_HIGHMEM], highmem[(unsigned)v[AK_HIGHMEM] % 5], v[AK_HIGHMEM] != 0);
        put_line(out, cap, &len, l[AK_MSCACHE], mscache[(unsigned)v[AK_MSCACHE] % 4], v[AK_MSCACHE] != 0);
    }
    put_line(out, cap, &len, l[AK_INFERNOCACHE], inferno[(unsigned)v[AK_INFERNOCACHE] % 3], v[AK_INFERNOCACHE] != 0);
    B(AK_DISABLEPAUSE, "disablepause");
    B(AK_OLDPLUGIN, "oldplugin");
    B(AK_HIBBLOCK, "hibblock");
    put_line(out, cap, &len, l[AK_SKIPLOGOS], logos[(unsigned)v[AK_SKIPLOGOS] % 4], v[AK_SKIPLOGOS] != 0);
    put_line(out, cap, &len, l[AK_HIDEPICS], pics[(unsigned)v[AK_HIDEPICS] % 4], v[AK_HIDEPICS] != 0);
    B(AK_HIDEMAC, "hidemac");
    B(AK_HIDEDLC, "hidedlc");
    B(AK_NOLED, "noled");
    B(AK_NOUMD, "noumd");
    if (!ark4) B(AK_DEADEF, "deadef"); /* not in ARK-4 */
    B(AK_NOANALOG, "noanalog");
    if (!ark4) B(AK_VITAMUTE, "vitamute");
    B(AK_QAFLAGS, "qaflags");
#undef B
    static const char *const regions[] = {"", "jp", "us", "eu"};
    char line[LINE_LEN];
    if (v[AK_REGION] >= 1 && v[AK_REGION] <= 3) {
        snprintf(line, sizeof(line), "vsh, region_%s, on\n", regions[v[AK_REGION]]);
        append(out, cap, &len, line);
    }
    if (v[AK_FAKEREGION] > 0) {
        snprintf(line, sizeof(line), "vsh, fakeregion_%d, on\n", v[AK_FAKEREGION]);
        append(out, cap, &len, line);
    }
    if (extra && extra[0]) {
        append(out, cap, &len, "\n");
        append(out, cap, &len, extra);
    }
    return len;
}

void arkcfg_set(ArkCfg *c, int key, int value)
{
    if (key < 0 || key >= AK_COUNT)
        return;
    if (!c->value[key] && value && c->level[key] == RL_OFF)
        c->level[key] = RL_ALWAYS;
    if (c->level[key] == RL_OFF)
        c->level[key] = key == AK_REGION || key == AK_FAKEREGION ? RL_VSH : RL_ALWAYS;
    c->value[key] = value;
}
