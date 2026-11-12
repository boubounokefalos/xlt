/* XLT - Settings screen.
 *
 * Categories on the left, their settings on the right, and a description of
 * the focused setting under them. ↑/↓ move, →/✕ go into a category, ←/→
 * change a value (✕ steps it forward or runs an action), L/R switch
 * category, ○ goes back. Sections, in order:
 *
 *   XLT              start at boot (ARK's launcher setting), default ISO
 *                      driver, removed entries, rescan
 *   Games              ARK: CPU speed, extra memory, caches, logos, UMD region,
 *                      Pause Game (Go)
 *   Plugins            every global line of ARK's PLUGINS.TXT files
 *   Power & Display    screensaver, USB charging, LEDs, backlight auto-off, auto sleep,
 *                      keeping the paused game (Go)
 *   Sound & Controls   AVLS, enter button, analog stick
 *   Network            WPA2, ad hoc channel, Wi-Fi power save, MAC address
 *   System             language, date and time formats, time zone, summer
 *                      time, UMD drive
 *   About              model, firmware, ARK, nickname
 *
 * Settings that only change the XMB are not shown (the XMB's CPU speed,
 * hidden game pictures and DLC, QA flags, the XMB region, Vita-only keys);
 * their lines are written back exactly as they were.
 *
 * Files are read on the background worker when the screen opens (the rows
 * show "..." until then). ARK and plugin changes are written when the screen
 * closes; ARK applies them the next time a game starts. PSP settings are in
 * the system registry: a change is written at once and read back; most apply
 * fully after a restart. A PSP setting whose registry key can't be read is
 * left out, so a firmware without it never sees a broken row.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "settings.h"
#include "sfx.h"
#include "ui_internal.h"
#include "anim.h"
#include "ark.h"
#include "bg.h"
#include "gamecfg.h"
#include "audiofmt.h"
#include "launch.h"
#include "library.h"
#include "history.h"
#include "arklauncher.h"
#include "version.h"
#include "plugins.h"
#include "psreg.h"
#include "saver.h"
#include "sysctl.h"

#define FOOTER_Y 248
#define TOP_Y 32
#define ROW 24
#define CAT_ROW 21 /* eight sections fit above the description */
#define CAT_X 12
#define CAT_W 116
#define ITEM_X 136
#define ITEM_W (SCREEN_W - 12 - ITEM_X)
#define VISIBLE 7                         /* item rows on screen */
#define DESC_Y (TOP_Y + VISIBLE * ROW + 8) /* description under the rows */
#define MAX_ITEMS 40
#define EXTRA_CAP (12 * 1024)

/* ------------------------------------------------------------------ model */

enum { SRC_ARK, SRC_REG, SRC_PLUG, SRC_XLT, SRC_ACTION, SRC_INFO };
enum { X_DRIVER, X_SAVER, X_SOUNDS, X_THEME, X_AUTOPLAY, X_LEVEL };
static const char *const O_LEVEL[LEVEL_COUNT] = {"Off", "By song", "By album"};
enum { A_REMOVED, A_RESCAN, A_SAVER, A_CLEAR_HISTORY };
static int s_clear_armed; /* ✕ once on Clear play history: the next ✕ clears */
enum { C_XLT, C_GAMES, C_PLUGINS, C_POWER, C_SOUND, C_NETWORK, C_SYSTEM, C_ABOUT, C_COUNT };
_Static_assert(C_COUNT == SETTINGS_SECTIONS, "settings.h's section count");

static const struct {
    const char *name, *sub;
    int icon;
} SECTIONS[C_COUNT] = {
    {"XLT", "Start, ISO driver, rescan", ICON_GRID},
    {"Games", "CPU, memory, caches", ICON_GAMES},
    {"Plugins", "On, off, where they load", ICON_PLUGIN},
    {"Power & Display", "Screensaver, sleep, LEDs", ICON_SUN},
    {"Sound & Controls", "AVLS, buttons, stick", ICON_VOLUME},
    {"Network", "Wi-Fi, ad hoc", ICON_WIFI},
    {"System", "Language, date, time", ICON_SETTINGS},
    {"About", "Model, firmware, ARK", ICON_INFO},
};

typedef struct {
    const char *label;
    const char *desc;
    int src, key;
    const char *const *opts; /* SRC_ARK / SRC_XLT choices */
    int nopts;
    char text[72];           /* SRC_INFO value, or a plugin's name */
} Item;

typedef struct {
    const char *name;
    const char *note; /* shown when the category itself is focused */
    Item items[MAX_ITEMS];
    int n;
} Cat;

/* PSP settings in the registry. vals/labels: the choices; a value read that
 * isn't one of them is shown as it is and replaced on the first change. */
typedef struct {
    const char *dir, *key;
    const char *label, *desc;
    int cat;
    int impose;          /* sceImpose param to update at once, 0 = none */
    const int *vals;
    const char *const *labels;
    int n;
} RegDef;

static const int V_OFFON[] = {0, 1};
static const char *const L_OFFON[] = {"Off", "On"};
static const int V_LANG[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const char *const L_LANG[] = {"Japanese", "English", "French", "Spanish", "German", "Italian",
                                     "Dutch", "Portuguese", "Russian", "Korean", "Chinese (Trad.)",
                                     "Chinese (Simp.)"};
static const int V_ENTER[] = {1, 0};
static const char *const L_ENTER[] = {"Cross (X)", "Circle (O)"};
static const int V_DATE[] = {0, 1, 2};
static const char *const L_DATE[] = {"YYYY/MM/DD", "MM/DD/YYYY", "DD/MM/YYYY"};
static const int V_TIME[] = {0, 1};
static const char *const L_TIME[] = {"24-hour", "12-hour"};
static const int V_LIGHT[] = {0, 30, 60, 180, 300, 600};
static const char *const L_LIGHT[] = {"Off", "30 seconds", "1 minute", "3 minutes", "5 minutes", "10 minutes"};
static const int V_SLEEP[] = {0, 60, 180, 300, 600, 900, 1800, 3600};
static const char *const L_SLEEP[] = {"Off", "1 minute", "3 minutes", "5 minutes", "10 minutes", "15 minutes",
                                      "30 minutes", "1 hour"};
static const int V_ADHOC[] = {0, 1, 6, 11};
static const char *const L_ADHOC[] = {"Automatic", "1", "6", "11"};
static const int V_TZ[] = {-720, -660, -600, -570, -540, -480, -420, -360, -300, -240, -210, -180, -120, -60,
                           0, 60, 120, 180, 210, 240, 270, 300, 330, 345, 360, 390, 420, 480, 540, 570, 600,
                           630, 660, 720, 780, 840};
#define TZ_COUNT ((int)(sizeof(V_TZ) / sizeof(V_TZ[0])))
static char s_tz_text[TZ_COUNT][16];
static const char *s_tz_labels[TZ_COUNT];

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const RegDef REG[] = {
    {"/CONFIG/SYSTEM/POWER_SAVING", "backlight_off_interval", "Backlight auto-off",
     "Turns the screen off after this long without a button press.", C_POWER, 0, V_LIGHT, L_LIGHT, N(V_LIGHT)},
    {"/CONFIG/SYSTEM/POWER_SAVING", "suspend_interval", "Auto sleep",
     "Puts the PSP to sleep after this long without a button press.", C_POWER, 0, V_SLEEP, L_SLEEP, N(V_SLEEP)},
    {"/CONFIG/SYSTEM/SOUND", "avls", "AVLS",
     "Automatic Volume Limiter: caps the headphone volume.", C_SOUND, 0x10, V_OFFON, L_OFFON, 2},
    {"/CONFIG/SYSTEM/XMB", "button_assign", "Enter button",
     "Which button confirms in the PSP's own dialogs (saving, network, keyboard).", C_SOUND, 0, V_ENTER, L_ENTER,
     2},
    {"/CONFIG/NETWORK/ADHOC", "channel", "Ad hoc channel",
     "Wi-Fi channel for local multiplayer. Automatic picks a free one.", C_NETWORK, 0, V_ADHOC, L_ADHOC,
     N(V_ADHOC)},
    {"/CONFIG/SYSTEM/POWER_SAVING", "wlan_mode", "Wi-Fi power save",
     "Saves battery in online games, at the cost of a slower connection.", C_NETWORK, 0, V_OFFON, L_OFFON, 2},
    {"/CONFIG/SYSTEM/XMB", "language", "Language",
     "The system language. Games that follow it start in this language.", C_SYSTEM, 0, V_LANG, L_LANG,
     N(V_LANG)},
    {"/CONFIG/DATE", "date_format", "Date format", "How games and system dialogs show dates.", C_SYSTEM, 0,
     V_DATE, L_DATE, N(V_DATE)},
    {"/CONFIG/DATE", "time_format", "Time format", "How games and system dialogs show the time.", C_SYSTEM, 0,
     V_TIME, L_TIME, N(V_TIME)},
    {"/CONFIG/DATE", "time_zone_offset", "Time zone", "Your offset from UTC. The clock follows it.", C_SYSTEM, 0,
     V_TZ, NULL, TZ_COUNT},
    {"/CONFIG/DATE", "summer_time", "Summer time", "Daylight saving time: one hour ahead.", C_SYSTEM, 0, V_OFFON,
     L_OFFON, 2},
};
#define NREG N(REG)

/* ARK choices */
static const char *const O_OFFON[] = {"Off", "On"};
static const char *const O_CLOCK[] = {"Auto", "133 MHz", "222 MHz", "333 MHz", "383 MHz", "403 MHz", "423 MHz",
                                      "443 MHz"};
static const char *const O_HIGHMEM[] = {"Off", "On", "Auto", "+16 MiB", "Max"};
static const char *const O_MSCACHE[] = {"Off", "4 KiB", "8 KiB", "16 KiB"};
static const char *const O_INFERNO[] = {"Off", "LRU", "RR"};
static const char *const O_LOGOS[] = {"Off", "All", "Game boot", "Cold boot"};
static const char *const O_REGION[] = {"Default", "Japan", "America", "Europe"};
static const char *const O_DRIVER[] = {"NP9660", "Inferno", "March33"};
static const int DRIVERS[] = {DRIVER_NP9660, DRIVER_INFERNO, DRIVER_MARCH33};
static const char *const O_SAVER[] = {"Off", "1 minute", "2 minutes", "5 minutes", "10 minutes"};
static const int SAVER_MIN[] = {0, 1, 2, 5, 10};

static struct {
    int open;
    Spring t;
    int cat, col;    /* col 0: categories, 1: items */
    int sel[C_COUNT], top[C_COUNT];
    int loaded, gen;
    ArkCfg ark, ark0;
    int ark_ok;
    PlugLine plug[PLUG_MAX];
    int plug_v[PLUG_MAX], plug_v0[PLUG_MAX]; /* PRL_*: 0 off, else the runlevel it is on for */
    int plug_rl[PLUG_MAX];                   /* runlevel the line was on (PRL_*, -1 = other) */
    int nplug;
    int reg[NREG], reg_ok[NREG];
    char nick[64];
    int driver0, saver0, sounds0, theme0, autoplay0, level0;
    int recent_dirty;
    Spring knob;     /* the focused switch's knob, Q8: 0 off .. 256 on */
    int knob_id;     /* which row it belongs to, -1 none */
} S;

static Cat s_cats[C_COUNT];
static char s_extra[EXTRA_CAP];

/* --------------------------------------------------------- worker jobs */

static struct {
    volatile int ready;
    int gen;
    ArkCfg ark;
    int ark_ok;
    PlugLine plug[PLUG_MAX];
    int nplug;
    int reg[NREG], reg_ok[NREG];
    char nick[64];
} L;
static char s_load_extra[EXTRA_CAP];

static void job_load(void *arg)
{
    L.ready = 0;
    L.gen = *(const int *)arg;
    L.ark_ok = ark_load_cfg(&L.ark, s_load_extra, sizeof(s_load_extra)) == 0;
    L.nplug = plugins_global(L.plug, PLUG_MAX);
    for (int i = 0; i < NREG; i++)
        L.reg_ok[i] = psreg_get_int(REG[i].dir, REG[i].key, &L.reg[i]) == 0;
    if (psreg_get_str("/CONFIG/SYSTEM", "owner_name", L.nick, sizeof(L.nick)) < 0)
        L.nick[0] = 0;
    L.ready = 1;
}

typedef struct {
    int save_ark;
    ArkCfg ark;
    int nplug;
} SaveArg;

/* Plugin changes for job_save; too big for a bg.c argument, so they wait
 * here (Settings can't reopen and load before the queued save has run). */
static struct {
    PlugLine p;
    const char *runlevel; /* NULL: keep the line's own */
    int on;
} s_save_plug[PLUG_MAX];

static volatile int s_save_err;
static char s_save_extra[EXTRA_CAP];

static void job_save(void *arg)
{
    const SaveArg *a = arg;
    if (a->save_ark && ark_save_cfg(&a->ark, s_save_extra) != 0)
        s_save_err |= 1;
    else if (a->save_ark)
        launcher_sync_now(); /* "Start XLT at boot" changed: XLT becomes ARK's launcher now, or ARK's menu returns */
    for (int i = 0; i < a->nplug; i++)
        if (plugins_global_set(&s_save_plug[i].p, s_save_plug[i].runlevel, s_save_plug[i].on) != 0)
            s_save_err |= 2;
}

static void job_save_ini(void *arg) { globalcfg_save(); }

/* ---------------------------------------------------------- building */

static Item *add(Cat *c, const char *label, const char *desc, int src, int key)
{
    if (c->n >= MAX_ITEMS)
        return NULL;
    Item *it = &c->items[c->n++];
    memset(it, 0, sizeof(*it));
    it->label = label;
    it->desc = desc;
    it->src = src;
    it->key = key;
    return it;
}

static void ark_item(Cat *c, int key, const char *label, const char *desc, const char *const *opts, int n)
{
    if (!ark_version())
        return;
    Item *it = add(c, label, desc, SRC_ARK, key);
    if (it) {
        it->opts = opts;
        it->nopts = n;
    }
}

static void info_item(Cat *c, const char *label, const char *value)
{
    Item *it = add(c, label, NULL, SRC_INFO, 0);
    if (it)
        snprintf(it->text, sizeof(it->text), "%s", value);
}

static void reg_items(Cat *c, int cat)
{
    for (int i = 0; i < NREG; i++)
        if (REG[i].cat == cat && (!S.loaded || S.reg_ok[i]))
            add(c, REG[i].label, REG[i].desc, SRC_REG, i);
}

/* Where a plugin loads: arkMenu's runlevels. 0 = off. */
enum { PRL_OFF, PRL_ALWAYS, PRL_GAME, PRL_UMD, PRL_HOMEBREW, PRL_POPS, PRL_VSH, PRL_LAUNCHER, PRL_COUNT };
static const char *const PRL_KEY[PRL_COUNT] = {"", "always", "game", "umd", "homebrew", "pops", "vsh", "launcher"};
static const char *const PRL_LABEL[PRL_COUNT] = {"Off", "Always", "All games", "PSP games", "Homebrew",
                                                 "PS1 games", "XMB", "Launcher"};
static const char *const PRL_DESC[PRL_COUNT] = {
    "Not loaded.",
    "Loads everywhere: games, homebrew, PS1 games and the XMB.",
    "Loads for every game: PSP, PS1 and homebrew.",
    "Loads for PSP games: UMD, ISO and PSN.",
    "Loads for homebrew only.",
    "Loads for PS1 games only.",
    "Loads in the XMB only.",
    "Loads in the launcher (XLT, arkMenu) only.",
};

static int prl_of(const char *rl)
{
    if (!strcmp(rl, "always") || !strcmp(rl, "all")) return PRL_ALWAYS;
    if (!strcmp(rl, "game")) return PRL_GAME;
    if (!strcmp(rl, "umd") || !strcmp(rl, "psp")) return PRL_UMD;
    if (!strcmp(rl, "homebrew")) return PRL_HOMEBREW;
    if (!strcmp(rl, "pops") || !strcmp(rl, "psx") || !strcmp(rl, "ps1")) return PRL_POPS;
    if (!strcmp(rl, "vsh") || !strcmp(rl, "xmb")) return PRL_VSH;
    if (!strcmp(rl, "launcher")) return PRL_LAUNCHER;
    return -1;
}

/* "110.8 GB free of 119.3 GB", or why there is no number. */
static void storage_text(const char *dev, char *out, int n)
{
    uint64_t f, t;
    if (sysctl_storage(dev, &f, &t) != 0 || !t) {
        snprintf(out, n, sysctl_storage_date() ? "Not found" : "Not measured yet");
        return;
    }
    char a[16], b[16];
    format_size(a, sizeof(a), f);
    format_size(b, sizeof(b), t);
    snprintf(out, n, "%s free of %s", a, b);
}

/* Free and used space are counted once each time XLT starts, when About's rows
 * are first entered (not when About is only highlighted on the way past): a thread reads the
 * Memory Stick's FAT (sysctl_storage_count_*) while a dialog shows how far it got,
 * and the rest of XLT keeps running. Moving off About or ○ stops it. If the raw
 * stick can't be read, the PSP driver's own count is the way left: it holds the
 * whole PSP (~12 s the first time after XLT starts, instant later), so the
 * dialog says so and is drawn before it starts. */
enum { SP_IDLE, SP_COUNTING, SP_DONE, SP_DRIVER };
static int s_space;          /* SP_*: the dialog is up unless SP_IDLE (also declared above settings_input) */
static int s_space_frames;   /* frames the dialog has been up */
static int s_space_raw = 1;  /* the raw count works (until it fails once) */
static int s_measured_once;  /* the driver counted since XLT started: later ones are instant */
static int s_counted;        /* counted since XLT started: About shows that from then on */
static Spring s_space_t;     /* the dialog: 0 hidden .. FX(1) shown */

static void storage_items(Cat *c)
{
    char buf[72];
    storage_text("ms0:", buf, sizeof(buf));
    Item *it = add(c, "Memory Stick", "Counted the first time About is opened after XLT starts.", SRC_INFO, 0);
    if (it)
        snprintf(it->text, sizeof(it->text), "%s", buf);
    uint64_t f, t;
    if (sysctl_storage("ms0:", &f, &t) == 0 && t && f <= t) {
        char u[16];
        format_size(u, sizeof(u), t - f);
        snprintf(buf, sizeof(buf), "%s (%d %%)", u, (int)((t - f) * 100 / t));
        info_item(c, "Used", buf);
    }
    if (sysctl_storage("ef0:", &f, &t) == 0 && t) { /* PSP Go */
        storage_text("ef0:", buf, sizeof(buf));
        info_item(c, "Internal storage", buf);
    }
}

static void build(void)
{
    memset(s_cats, 0, sizeof(s_cats));
    int ark = ark_version(), ark4 = ark == 4, model = sysctl_model();
    int go = model == 4, psp1000 = model == 0;
    Cat *c;

    c = &s_cats[C_XLT];
    c->name = SECTIONS[C_XLT].name;
    c->note = "How XLT looks, starts and finds your games.";
    add(c, "Theme", "The colours of XLT. Changes at once, so you can look before you leave.", SRC_XLT, X_THEME);
    add(c, "Play the next video", "When a video ends, the next one in its folder starts after a few seconds "
        "(cancel with Circle).", SRC_XLT, X_AUTOPLAY);
    ark_item(c, AK_LAUNCHER, "Start XLT at boot",
             "ARK opens XLT instead of the XMB when the PSP starts.", O_OFFON, 2);
    Item *it = add(c, "Default ISO driver",
                   "Used for disc images without their own choice (Triangle on a game sets one).", SRC_XLT, X_DRIVER);
    if (it) { it->opts = O_DRIVER; it->nopts = 3; }
    it = add(c, "Removed entries", "Pictures, music and video you removed. Restore them from this list.",
             SRC_ACTION, A_REMOVED);
    if (it) snprintf(it->text, sizeof(it->text), "%d", library_count(VIEW_HIDDEN));
    add(c, "Rescan library", "Looks for new and removed games, music, video and pictures now.", SRC_ACTION,
        A_RESCAN);
    it = add(c, "Clear play history",
             "Forgets which games and apps you played: the Recent rows start over. Favorites stay first.",
             SRC_ACTION, A_CLEAR_HISTORY);
    if (it) snprintf(it->text, sizeof(it->text), "%d", history_count());

    c = &s_cats[C_GAMES];
    c->name = SECTIONS[C_GAMES].name;
    c->note = ark ? "ARK settings for every game. They apply the next time a game starts."
                  : "These settings need ARK.";
    ark_item(c, AK_CLOCK_GAME, "CPU speed",
             "Clock for every game without its own speed. Above 333 MHz is overclocking.", O_CLOCK,
             ark4 ? 4 : ACLK_COUNT);
    if (!psp1000)
        ark_item(c, AK_HIGHMEM, "Extra memory",
                 "Gives homebrew and some games the PSP-2000+'s extra 32 MiB. Auto lets ARK decide.",
                 ark4 ? O_OFFON : O_HIGHMEM, ark4 ? 2 : 5);
    ark_item(c, AK_MSCACHE, "Memory Stick cache", "Caches reads from the Memory Stick: faster loading.",
             ark4 ? O_OFFON : O_MSCACHE, ark4 ? 2 : 4);
    ark_item(c, AK_INFERNOCACHE, "ISO cache (Inferno)", "Read cache of the Inferno ISO driver.", O_INFERNO, 3);
    ark_item(c, AK_SKIPLOGOS, "Skip logos", "Skips the Sony boot logo, the game start animation, or both.",
             O_LOGOS, 4);
    ark_item(c, AK_REGION, "UMD region", "Region UMD games and video see. Default keeps the PSP's own.",
             O_REGION, 4);
    if (go)
        ark_item(c, AK_DISABLEPAUSE, "Disable Pause Game", "Turns off the PSP Go's Pause Game feature.",
                 O_OFFON, 2);

    c = &s_cats[C_PLUGINS];
    c->name = SECTIONS[C_PLUGINS].name;
    c->note = ark ? "ARK's PLUGINS.TXT. Changes apply the next time a game starts; Triangle on a game sets them per game."
                  : "Plugins need ARK.";
    for (int i = 0; i < S.nplug; i++) {
        it = add(c, NULL, NULL, SRC_PLUG, i);
        if (!it)
            break;
        const char *base = strrchr(S.plug[i].path, '/');
        snprintf(it->text, sizeof(it->text), "%s", base ? base + 1 : S.plug[i].path);
    }
    if (go)
        ark_item(c, AK_OLDPLUGIN, "Old plugin support",
                 "Loads plugins from ms0:/SEPLUGINS on the PSP Go, like older firmware did.", O_OFFON, 2);
    if (S.loaded && !c->n)
        info_item(c, ark ? "No plugins in PLUGINS.TXT" : "Needs ARK", "");

    c = &s_cats[C_POWER];
    c->name = SECTIONS[C_POWER].name;
    c->note = "Battery, screen and lights.";
    it = add(c, "Screensaver", "Shows the time and date after this long without a button press, and dims the "
             "screen. Any button wakes it.", SRC_XLT, X_SAVER);
    if (it) { it->opts = O_SAVER; it->nopts = COUNT(O_SAVER); }
    add(c, "Show screensaver", "Shows it now, to try it out. Any button wakes it.", SRC_ACTION, A_SAVER);
    if (!psp1000)
        ark_item(c, AK_USBCHARGE, "USB charging", "Charges the battery over the USB cable.", O_OFFON, 2);
    reg_items(c, C_POWER);
    ark_item(c, AK_NOLED, "Turn off LEDs", "Keeps the power, Memory Stick and Wi-Fi lights off.", O_OFFON, 2);
    if (go)
        ark_item(c, AK_HIBBLOCK, "Keep the paused game",
                 "Keeps the PSP Go's paused game when other software starts.", O_OFFON, 2);

    c = &s_cats[C_SOUND];
    c->name = SECTIONS[C_SOUND].name;
    c->note = "Volume limiter and buttons.";
    add(c, "Interface sounds", "Soft clicks and chimes as you move around XLT and start a game. They play "
        "over music and follow the PSP's volume.", SRC_XLT, X_SOUNDS);
    add(c, "Even out music volume", "Turns loud songs down to match quieter ones, from their ReplayGain or "
        "Sound Check tags. Never louder than without it; AVLS still limits the headphones after it.",
        SRC_XLT, X_LEVEL);
    reg_items(c, C_SOUND);
    ark_item(c, AK_NOANALOG, "Disable analog stick", "Ignores the analog stick, for a worn one that drifts.",
             O_OFFON, 2);

    c = &s_cats[C_NETWORK];
    c->name = SECTIONS[C_NETWORK].name;
    c->note = "Wi-Fi for games.";
    ark_item(c, AK_WPA2, "WPA2 for Wi-Fi", "Lets games join WPA2 networks (the PSP only knows WPA).", O_OFFON, 2);
    reg_items(c, C_NETWORK);
    ark_item(c, AK_HIDEMAC, "Hide MAC address", "Shows a made-up MAC address instead of the PSP's own.",
             O_OFFON, 2);

    c = &s_cats[C_SYSTEM];
    c->name = SECTIONS[C_SYSTEM].name;
    c->note = "The PSP's own settings that games use. Most apply fully after a restart.";
    reg_items(c, C_SYSTEM);
    if (!go)
        ark_item(c, AK_NOUMD, "Disable UMD drive", "Turns the UMD drive off: saves battery when you only use ISOs.",
                 O_OFFON, 2);

    c = &s_cats[C_ABOUT];
    c->name = SECTIONS[C_ABOUT].name;
    c->note = "This PSP.";
    char buf[48];
    info_item(c, "Model", sysctl_model_name());
    sysctl_firmware(buf, sizeof(buf));
    info_item(c, "System software", buf);
    if (ark)
        snprintf(buf, sizeof(buf), "ARK-%d", ark);
    else
        snprintf(buf, sizeof(buf), "Not found");
    info_item(c, "Custom firmware", buf);
    if (S.nick[0])
        info_item(c, "Nickname", S.nick);
    info_item(c, "Launcher", "XLT " XLT_VERSION);
    storage_items(c);

    for (int i = 0; i < C_COUNT; i++)
        if (!s_cats[i].n && (S.loaded || i == C_GAMES || i == C_NETWORK))
            info_item(&s_cats[i], ark ? "Nothing to set here" : "Needs ARK", "");
    for (int i = 0; i < C_COUNT; i++) {
        if (S.sel[i] >= s_cats[i].n) S.sel[i] = s_cats[i].n ? s_cats[i].n - 1 : 0;
        if (S.top[i] > S.sel[i]) S.top[i] = S.sel[i];
    }
}

/* ---------------------------------------------------------- values */

static int driver_index(int d)
{
    for (int i = 0; i < 3; i++)
        if (DRIVERS[i] == d)
            return i;
    return 0;
}

static int saver_index(int m)
{
    for (int i = COUNT(SAVER_MIN) - 1; i > 0; i--)
        if (m >= SAVER_MIN[i])
            return i;
    return 0;
}

static int reg_index(int r)
{
    for (int i = 0; i < REG[r].n; i++)
        if (REG[r].vals[i] == S.reg[r])
            return i;
    return -1;
}

static int item_ready(const Item *it)
{
    return it->src == SRC_INFO || it->src == SRC_ACTION || it->src == SRC_XLT || S.loaded;
}

static void item_value(const Item *it, char *out, int n)
{
    out[0] = 0;
    if (!item_ready(it)) {
        snprintf(out, n, "...");
        return;
    }
    switch (it->src) {
    case SRC_ARK: {
        int v = S.ark.value[it->key];
        snprintf(out, n, "%s", v >= 0 && v < it->nopts ? it->opts[v] : "?");
        int lv = S.ark.level[it->key];
        if (v && it->nopts == 2 && lv != RL_ALWAYS && lv != RL_OFF) { /* on for some software only */
            static const char *const rl[] = {"", "", "games", "UMD", "homebrew", "PS1", "XMB", "launcher"};
            snprintf(out, n, "On (%s)", lv < 8 ? rl[lv] : "?");
        }
        break;
    }
    case SRC_REG: {
        int i = reg_index(it->key);
        const char *const *labels = REG[it->key].labels ? REG[it->key].labels : s_tz_labels;
        if (i >= 0) snprintf(out, n, "%s", labels[i]);
        else snprintf(out, n, "%d", S.reg[it->key]);
        break;
    }
    case SRC_PLUG: {
        int v = S.plug_v[it->key];
        snprintf(out, n, "%s", v >= 0 ? PRL_LABEL[v] : S.plug[it->key].runlevel);
        break;
    }
    case SRC_XLT:
        if (it->key == X_THEME)
            snprintf(out, n, "%s", g_th->name);
        else if (it->key == X_SOUNDS)
            snprintf(out, n, "%s", g_cfg.ui_sounds ? "On" : "Off");
        else if (it->key == X_AUTOPLAY)
            snprintf(out, n, "%s", g_cfg.autoplay ? "On" : "Off");
        else if (it->key == X_LEVEL)
            snprintf(out, n, "%s", O_LEVEL[g_cfg.level >= 0 && g_cfg.level < LEVEL_COUNT ? g_cfg.level : 0]);
        else if (it->key == X_SAVER)
            snprintf(out, n, "%s", O_SAVER[saver_index(g_cfg.saver_min)]);
        else
            snprintf(out, n, "%s", O_DRIVER[driver_index(g_cfg.iso_driver)]);
        break;
    case SRC_ACTION:
    case SRC_INFO:
        snprintf(out, n, "%s", it->text);
        break;
    }
}

/* On/off rows are drawn as a switch: 0 or 1, or -1 for any other row. */
static int switch_state(const Item *it)
{
    if (it->src == SRC_ARK && it->nopts == 2 && it->opts == O_OFFON)
        return S.ark.value[it->key] > 0;
    if (it->src == SRC_REG && REG[it->key].labels == L_OFFON && REG[it->key].n == 2)
        return reg_index(it->key) == 1;
    if (it->src == SRC_XLT && it->key == X_SOUNDS)
        return g_cfg.ui_sounds;
    if (it->src == SRC_XLT && it->key == X_AUTOPLAY)
        return g_cfg.autoplay;
    return -1;
}

static void change(Item *it, int dir)
{
    if (!item_ready(it))
        return;
    int was = switch_state(it);
    switch (it->src) {
    case SRC_ARK: {
        int v = (S.ark.value[it->key] + dir + it->nopts) % it->nopts;
        arkcfg_set(&S.ark, it->key, v);
        break;
    }
    case SRC_REG: {
        const RegDef *r = &REG[it->key];
        int i = reg_index(it->key);
        i = i < 0 ? 0 : (i + dir + r->n) % r->n;
        if (psreg_set_int(r->dir, r->key, r->vals[i]) == 0) {
            S.reg[it->key] = r->vals[i];
            system_read_formats(); /* the clock follows a new time format at once */
            if (r->impose)
                sysctl_impose_set(r->impose, r->vals[i]); /* at once, not only after a restart */
        } else {
            ui_toast("Couldn't change that PSP setting");
        }
        break;
    }
    case SRC_PLUG: { /* Off -> Always -> All games -> ... -> Launcher -> Off */
        int v = S.plug_v[it->key];
        S.plug_v[it->key] = v < 0 ? (dir > 0 ? PRL_ALWAYS : PRL_OFF) : (v + dir + PRL_COUNT) % PRL_COUNT;
        break;
    }
    case SRC_XLT:
        if (it->key == X_THEME) {
            g_cfg.theme = (g_cfg.theme + dir + THEME_COUNT) % THEME_COUNT;
            theme_set(g_cfg.theme); /* everything is drawn with it from the next frame */
        } else if (it->key == X_SOUNDS)
            g_cfg.ui_sounds = !g_cfg.ui_sounds;
        else if (it->key == X_AUTOPLAY)
            g_cfg.autoplay = !g_cfg.autoplay;
        else if (it->key == X_LEVEL) /* the playing song follows at its next block */
            g_cfg.level = (g_cfg.level + dir + LEVEL_COUNT) % LEVEL_COUNT;
        else if (it->key == X_SAVER)
            g_cfg.saver_min = SAVER_MIN[(saver_index(g_cfg.saver_min) + dir + COUNT(SAVER_MIN)) % COUNT(SAVER_MIN)];
        else
            g_cfg.iso_driver = DRIVERS[(driver_index(g_cfg.iso_driver) + dir + 3) % 3];
        break;
    }
    int now = switch_state(it);
    if (was >= 0 && now != was)
        sfx_play(now ? SFX_ON : SFX_OFF); /* turning Interface sounds off: already silent */
    else if (was < 0)
        sfx_play(SFX_MOVE);
}

/* ------------------------------------------------------------ open/close */

/* The section the user went into moves to the front of the home row. */
static void touch_section(int c)
{
    int *r = g_cfg.set_recent, i = 0;
    while (i < SETTINGS_RECENT - 1 && r[i] != c)
        i++;
    if (i == 0 && r[0] == c)
        return;
    for (; i > 0; i--)
        r[i] = r[i - 1];
    r[0] = c;
    S.recent_dirty = 1;
}

int settings_recent(int i) { return i >= 0 && i < SETTINGS_RECENT ? g_cfg.set_recent[i] % C_COUNT : 0; }
const char *settings_section_name(int c) { return SECTIONS[c % C_COUNT].name; }
const char *settings_section_sub(int c) { return SECTIONS[c % C_COUNT].sub; }
int settings_section_icon(int c) { return SECTIONS[c % C_COUNT].icon; }

void settings_open(void)
{
    s_clear_armed = 0;
    S.open = 1;
    S.knob_id = -1;
    S.col = 0;
    S.loaded = 0;
    S.nplug = 0;
    S.driver0 = g_cfg.iso_driver;
    S.saver0 = g_cfg.saver_min;
    S.sounds0 = g_cfg.ui_sounds;
    S.autoplay0 = g_cfg.autoplay;
    S.level0 = g_cfg.level;
    S.theme0 = g_cfg.theme;
    if (!s_tz_labels[0])
        for (int i = 0; i < TZ_COUNT; i++) {
            int m = V_TZ[i] < 0 ? -V_TZ[i] : V_TZ[i];
            snprintf(s_tz_text[i], sizeof(s_tz_text[i]), "UTC%c%d:%02d", V_TZ[i] < 0 ? '-' : '+', m / 60, m % 60);
            s_tz_labels[i] = s_tz_text[i];
        }
    build();
    spring_to(&S.t, FX(1));
    static int gen;
    S.gen = ++gen;
    bg_post(job_load, &S.gen, sizeof(S.gen));
}

static void close_settings(void)
{
    if (S.loaded) {
        static SaveArg a;
        memset(&a, 0, sizeof(a));
        a.save_ark = S.ark_ok && memcmp(&S.ark, &S.ark0, sizeof(ArkCfg)) != 0;
        a.ark = S.ark;
        for (int i = 0; i < S.nplug; i++)
            if (S.plug_v[i] != S.plug_v0[i]) {
                int v = S.plug_v[i];
                s_save_plug[a.nplug].p = S.plug[i];
                s_save_plug[a.nplug].on = v != PRL_OFF;
                /* off keeps the line's runlevel; a new runlevel is written as arkMenu spells it */
                s_save_plug[a.nplug].runlevel = v > PRL_OFF && v != S.plug_rl[i] ? PRL_KEY[v] : NULL;
                a.nplug++;
                S.plug_v0[i] = v;
                if (v > PRL_OFF) S.plug_rl[i] = v;
            }
        if (a.save_ark || a.nplug) {
            memcpy(s_save_extra, s_extra, sizeof(s_extra));
            S.ark0 = S.ark;
            bg_post(job_save, &a, sizeof(a));
        }
    }
    if (g_cfg.iso_driver != S.driver0 || g_cfg.saver_min != S.saver0 || g_cfg.ui_sounds != S.sounds0 ||
        g_cfg.theme != S.theme0 || g_cfg.autoplay != S.autoplay0 || g_cfg.level != S.level0 ||
        S.recent_dirty)
        bg_post(job_save_ini, NULL, 0);
    S.recent_dirty = 0;
    S.open = 0;
    spring_to(&S.t, 0);
}

void settings_open_section(int c)
{
    settings_open();
    S.cat = c % C_COUNT;
    S.col = s_cats[S.cat].n ? 1 : 0;
    touch_section(S.cat);
}

int settings_is_open(void) { return S.open || S.t.x > 0; }
int settings_covers(void) { return S.open && S.t.x >= FX(1); }

/* ------------------------------------------------------------------ input */

static void select_item(int i)
{
    Cat *c = &s_cats[S.cat];
    s_clear_armed = 0;
    if (i < 0) i = 0;
    if (i >= c->n) i = c->n - 1;
    if (i < 0) i = 0;
    if (i != S.sel[S.cat]) sfx_play(SFX_MOVE);
    S.sel[S.cat] = i;
    if (i < S.top[S.cat]) S.top[S.cat] = i;
    if (i >= S.top[S.cat] + VISIBLE) S.top[S.cat] = i - VISIBLE + 1;
}

static void run_action(Item *it)
{
    if (it->key == A_REMOVED) {
        if (library_count(VIEW_HIDDEN) == 0) {
            ui_toast("Nothing is removed from the library");
            return;
        }
        close_settings();
        ui_open_removed();
    } else if (it->key == A_SAVER) {
        saver_start();

    } else if (it->key == A_CLEAR_HISTORY) {
        if (history_count() == 0) {
            ui_toast("Nothing played yet");
        } else if (!s_clear_armed) {
            s_clear_armed = 1;
            ui_toast("Press X again to clear the play history");
        } else {
            s_clear_armed = 0;
            history_clear();
            library_resort(); /* the Recent rows and "Last played" follow */
            ui_library_changed();
            snprintf(it->text, sizeof(it->text), "0");
            ui_toast("Play history cleared");
        }
    } else if (it->key == A_RESCAN) {
        if (library_scanning()) {
            ui_toast("Already scanning");
        } else {
            library_rescan();
            ui_toast("Scanning the Memory Stick...");
        }
    }
}

static void space_cancel(void);
static int s_space;

int settings_input(unsigned int p)
{
    if (!S.open)
        return 0;
    if (s_space) { /* the free-space dialog: ○ stops it, leaving About does too (settings_step) */
        if (p & PSP_CTRL_CIRCLE) { /* stops it, and back to the sections as ○ always goes */
            space_cancel();
            S.col = 0;
            sfx_play(SFX_CLOSE);
            return 1;
        }
        if (p & (PSP_CTRL_CROSS | PSP_CTRL_RIGHT | PSP_CTRL_LEFT | PSP_CTRL_TRIANGLE | PSP_CTRL_SQUARE)) return 1;
    }
    Cat *c = &s_cats[S.cat];
    if (p & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) {
        S.cat = (S.cat + (p & PSP_CTRL_LTRIGGER ? C_COUNT - 1 : 1)) % C_COUNT;
        sfx_play(SFX_CARD);
        if (S.col == 1) {
            if (s_cats[S.cat].n) touch_section(S.cat);
            else S.col = 0;
        }
        return 1;
    }
    if (S.col == 0) {
        if (p & PSP_CTRL_UP) { S.cat = (S.cat + C_COUNT - 1) % C_COUNT; sfx_play(SFX_MOVE); }
        else if (p & PSP_CTRL_DOWN) { S.cat = (S.cat + 1) % C_COUNT; sfx_play(SFX_MOVE); }
        else if (p & (PSP_CTRL_RIGHT | PSP_CTRL_CROSS) && c->n) {
            S.col = 1;
            touch_section(S.cat);
        }
        else if (p & PSP_CTRL_CIRCLE) close_settings();
        return 1;
    }
    if (!c->n) {
        S.col = 0;
        return 1;
    }
    Item *it = &c->items[S.sel[S.cat]];
    if (p & PSP_CTRL_UP) select_item(S.sel[S.cat] - 1);
    else if (p & PSP_CTRL_DOWN) select_item(S.sel[S.cat] + 1);
    else if (p & PSP_CTRL_CIRCLE) S.col = 0;
    else if (p & PSP_CTRL_LEFT) {
        if (it->src == SRC_INFO || it->src == SRC_ACTION) S.col = 0;
        else if (switch_state(it) != 0) change(it, -1); /* a switch: left is off */
    } else if (p & PSP_CTRL_RIGHT) {
        if (it->src != SRC_INFO && it->src != SRC_ACTION && switch_state(it) != 1) change(it, 1); /* right is on */
    } else if (p & PSP_CTRL_CROSS) {
        if (it->src == SRC_ACTION) run_action(it);
        else if (it->src != SRC_INFO) change(it, 1);
    }
    return 1;
}

static void measure_now(void)
{
    bg_flush(); /* nothing else may be using the stick meanwhile */
    int ms = sysctl_storage_measure();
    char msg[64];
    snprintf(msg, sizeof(msg), "free space measured in %d ms (%s)", ms, s_measured_once ? "again" : "first time");
    library_note(msg);
}

static int s_space_drawn;    /* what the dialog shows: its last state, while it fades out too */

static void space_close(void)
{
    s_space = SP_IDLE;
    spring_to(&s_space_t, 0);
}

static void space_start(void)
{
    s_space_frames = 0;
    if (s_space_raw && sysctl_storage_count_start() == 0) {
        s_space = SP_COUNTING;
    } else if (s_measured_once) { /* the driver remembers: instant, no dialog needed */
        measure_now();
        s_counted = 1;
        build();
        return;
    } else {
        s_space = SP_DRIVER;
    }
    spring_snap(&s_space_t, 0);
    spring_to(&s_space_t, FX(1));
    if (s_space == SP_DRIVER) /* the PSP stops on this frame: no fading in */
        spring_snap(&s_space_t, FX(1));
}

/* Stops a count still running (moving off About, ○). */
static void space_cancel(void)
{
    if (s_space == SP_COUNTING)
        sysctl_storage_count_cancel();
    space_close();
}

/* about: About is the section shown; inside: its rows are entered */
static int space_step(int about, int inside)
{
    int dirty = 0;
    if (inside && !s_counted && s_space == SP_IDLE) {
        space_start();
        dirty = 1;
    } else if (!about && s_space != SP_IDLE) {
        space_cancel();
        dirty = 1;
    }
    int old = s_space_t.x;
    spring_step(&s_space_t);
    dirty |= s_space_t.x != old;
    if (s_space == SP_IDLE)
        return dirty;
    s_space_drawn = s_space;
    s_space_frames++;
    if (s_space == SP_COUNTING) {
        static int shown_pm = -1;
        int pm, st = sysctl_storage_count_state(&pm);
        if (pm != shown_pm) { shown_pm = pm; dirty = 1; }
        if (st == COUNT_DONE || st == COUNT_FAILED) {
            int ms;
            const char *how = sysctl_storage_count_how(&ms);
            char msg[112];
            if (st == COUNT_DONE)
                snprintf(msg, sizeof(msg), "free space counted in %d ms from %s", ms, how);
            else
                snprintf(msg, sizeof(msg), "free space: the raw stick can't be read (%s), using the driver",
                         how[0] ? how : "not opened");
            library_note(msg);
            if (st == COUNT_DONE) {
                build();
                s_counted = 1;
                s_space = SP_DONE;
                s_space_frames = 0;
            } else {
                s_space_raw = 0; /* not again this time XLT runs */
                s_space = SP_DRIVER;
                s_space_frames = 0;
                spring_snap(&s_space_t, FX(1)); /* the PSP stops on this frame: fully shown */
            }
            dirty = 1;
        }
    } else if (s_space == SP_DONE) {
        if (s_space_frames >= 24) /* a quick count still shows the dialog a moment, not a flicker */
            space_close();
    } else if (s_space == SP_DRIVER) {
        /* the note is on screen first; then the driver holds the PSP until it's done */
        if (s_space_frames > 4) {
            measure_now();
            s_measured_once = 1;
            s_counted = 1;
            build();
            space_close();
        }
        dirty = 1;
    }
    if (s_space != SP_IDLE)
        s_space_drawn = s_space;
    return dirty;
}

int settings_step(void)
{
    int old = S.t.x;
    spring_step(&S.t);
    int dirty = S.t.x != old;
    int about = S.open && S.cat == C_ABOUT && S.t.x >= FX(1);
    dirty |= space_step(about, about && S.col == 1);
    {   /* the focused switch slides; any other row is drawn where it rests */
        Cat *c = &s_cats[S.cat];
        int id = -1, on = 0;
        if (S.col == 1 && c->n && item_ready(&c->items[S.sel[S.cat]])) {
            on = switch_state(&c->items[S.sel[S.cat]]);
            if (on >= 0) id = S.cat * 256 + S.sel[S.cat];
        }
        if (id != S.knob_id) {
            S.knob_id = id;
            spring_snap(&S.knob, on > 0 ? FX(1) : 0);
            dirty = 1;
        } else if (id >= 0) {
            spring_to(&S.knob, on > 0 ? FX(1) : 0);
        }
        old = S.knob.x;
        spring_step(&S.knob);
        dirty |= S.knob.x != old;
    }
    if (L.ready && L.gen == S.gen && !S.loaded && S.open) {
        S.ark = S.ark0 = L.ark;
        S.ark_ok = L.ark_ok;
        memcpy(s_extra, s_load_extra, sizeof(s_extra));
        S.nplug = L.nplug;
        memcpy(S.plug, L.plug, sizeof(PlugLine) * L.nplug);
        for (int i = 0; i < S.nplug; i++) {
            S.plug_rl[i] = prl_of(S.plug[i].runlevel);
            S.plug_v[i] = S.plug_v0[i] = S.plug[i].on ? S.plug_rl[i] : PRL_OFF;
        }
        memcpy(S.reg, L.reg, sizeof(S.reg));
        memcpy(S.reg_ok, L.reg_ok, sizeof(S.reg_ok));
        snprintf(S.nick, sizeof(S.nick), "%s", L.nick);
        S.loaded = 1;
        build(); /* plugins and PSP settings are known now */
        if (S.col == 1 && !s_cats[S.cat].n)
            S.col = 0;
        dirty = 1;
    }
    if (s_save_err) {
        ui_toast(s_save_err & 1 ? "Couldn't write ARK's SETTINGS.TXT" : "Couldn't write PLUGINS.TXT");
        s_save_err = 0;
        dirty = 1;
    }
    return dirty;
}

/* ------------------------------------------------------------------- draw */

#define SW_W 30
#define SW_H 16

/* An on/off switch ending at rx, centred on cy; t = knob position 0..256. */
static void draw_switch(int rx, int cy, int t, int focused)
{
    if (t < 0) t = 0;
    if (t > 256) t = 256;
    int x = rx - SW_W, y = cy - SW_H / 2, k = SW_H - 4;
    u32 track = focused ? color_lerp(C_ACCENT_DK, C_WHITE, t) : color_lerp(C_PILL, C_ACCENT, t);
    u32 knob = focused ? color_lerp(C_WHITE, C_ACCENT, t) : color_lerp(C_MUTED, C_WHITE, t);
    gfx_rrect(x, y, SW_W, SW_H, SW_H / 2, track);
    int kx = x + 2 + ((SW_W - 4 - k) * t >> 8);
    gfx_rrect(kx, y + 2, k, k, k / 2, knob);
}

/* Up to two lines of text wrapped at spaces into width w. */
static void draw_wrapped(int x, int y, int w, const char *s, u32 color)
{
    char line[160];
    int lh = gfx_line_h(FONT_SMALL);
    for (int ln = 0; ln < 2 && *s; ln++) {
        int best = 0, i = 0, len = (int)strlen(s);
        while (i <= len) {
            if (s[i] == ' ' || s[i] == 0) {
                snprintf(line, sizeof(line), "%.*s", i, s);
                if (gfx_text_width(FONT_SMALL, line) > w)
                    break;
                best = i;
                if (!s[i])
                    break;
            }
            i++;
        }
        if (best == 0 || ln == 1) { /* one long word, or the last line: cut with an ellipsis */
            gfx_text_fit(FONT_SMALL, x, y + ln * lh, w, s, color);
            return;
        }
        snprintf(line, sizeof(line), "%.*s", best, s);
        gfx_text(FONT_SMALL, x, y + ln * lh, line, color);
        s += best;
        while (*s == ' ')
            s++;
    }
}

/* The free-space dialog over About: what it does, how far it got, and ○ to stop. */
static void draw_space(int t)
{
    int a = s_space_t.x;
    if (a <= 0)
        return;
    if (a > 256) a = 256;
    a = a * t >> 8;
    gfx_set_opacity(a);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, WITH_ALPHA(0x000000, 150));
    const int w = 320, h = 118, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2 + (256 - a) * 12 / 256;
    gfx_rrect(x, y, w, h, 12, C_SURFACE);
    int fb = gfx_line_h(FONT_BOLD), fs = gfx_line_h(FONT_SMALL);
    gfx_text_center(FONT_BOLD, SCREEN_W / 2, y + 14, "Measuring free and used space", C_TEXT);
    int ty = y + 14 + fb + 4;
    if (s_space_drawn == SP_DRIVER) {
        gfx_text_center(FONT_SMALL, SCREEN_W / 2, ty, "This takes about 12 s the first time.", C_MUTED);
        gfx_text_center(FONT_SMALL, SCREEN_W / 2, ty + fs + 2, "The PSP won't respond until it's done.", C_MUTED);
    } else {
        int pm = 0;
        sysctl_storage_count_state(&pm);
        if (s_space_drawn == SP_DONE) pm = 1000;
        gfx_text_center(FONT_SMALL, SCREEN_W / 2, ty, s_space_drawn == SP_DONE ? "Done." : "Reading the Memory Stick's file table...",
                        C_MUTED);
        const int bx = x + 24, bw = w - 48 - 44, by = ty + fs + 12;
        gfx_rrect(bx, by, bw, 6, 3, C_RAISED);
        int fill = bw * pm / 1000;
        if (fill >= 6)
            gfx_rrect(bx, by, fill, 6, 3, C_ACCENT);
        char pct[16];
        snprintf(pct, sizeof(pct), "%d %%", pm / 10);
        gfx_text_right(FONT_SMALL, x + w - 24, by + 3 - fs / 2, pct, C_TEXT);
        static const Hint hint[] = {{ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Stop"}};
        if (s_space_drawn != SP_DONE)
            ui_draw_hints(hint, 1, SCREEN_W / 2, y + h - fs - 10);
    }
    gfx_set_opacity(t);
}

void settings_draw(const SysStatus *st)
{
    int t = S.t.x;
    if (t <= 0)
        return;
    if (t > 256) t = 256;
    gfx_set_opacity(t);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
    ui_draw_status_bar("Settings", st);
    int fb = gfx_line_h(FONT_BODY), fs = gfx_line_h(FONT_SMALL);

    /* categories */
    for (int i = 0; i < C_COUNT; i++) {
        int y = TOP_Y + i * CAT_ROW;
        int on = i == S.cat;
        if (on)
            gfx_rrect(CAT_X, y, CAT_W, CAT_ROW - 1, 5, S.col == 0 ? C_ACCENT : C_RAISED);
        gfx_text_fit(on ? FONT_BOLD : FONT_BODY, CAT_X + 8, y + (CAT_ROW - 1 - fb) / 2, CAT_W - 12, s_cats[i].name,
                     !on ? C_MUTED : S.col == 0 ? C_WHITE : C_TEXT); /* white only on the accent: the raised row is pale on Light */
    }
    gfx_rect(ITEM_X - 6, TOP_Y, 1, VISIBLE * ROW, C_LINE);

    /* items */
    Cat *c = &s_cats[S.cat];
    int sel = S.sel[S.cat], top = S.top[S.cat];
    for (int k = 0; k < VISIBLE && top + k < c->n; k++) {
        int i = top + k, y = TOP_Y + k * ROW;
        Item *it = &c->items[i];
        int focused = S.col == 1 && i == sel;
        if (focused)
            gfx_rrect(ITEM_X, y, ITEM_W, ROW - 2, 5, C_ACCENT);
        char v[48];
        item_value(it, v, sizeof(v));
        int sw = item_ready(it) ? switch_state(it) : -1;
        const char *note = NULL; /* "On (games)": the switch, and what it is on for */
        if (sw > 0 && !strncmp(v, "On (", 4)) {
            note = v + 4;
            v[strlen(v) - 1] = 0;
        }
        int vw = sw >= 0 ? SW_W + (note ? gfx_text_width(FONT_SMALL, note) + 6 : 0)
                         : gfx_text_width(FONT_SMALL, v) + (focused && (it->src != SRC_INFO && it->src != SRC_ACTION) ? 24 : 0);
        if (vw > ITEM_W / 2) vw = ITEM_W / 2;
        const char *label = it->src == SRC_PLUG ? it->text : it->label;
        int lx = ITEM_X + 8, lw = ITEM_W - 16 - vw - 8;
        if (it->src == SRC_PLUG) { /* the value says where it loads */
            gfx_text_fit(FONT_BODY, lx, y + (ROW - 2 - fb) / 2, lw, label,
                         focused ? C_WHITE : S.plug_v[it->key] == PRL_OFF ? C_MUTED : C_TEXT);
        } else {
            gfx_text_fit(FONT_BODY, lx, y + (ROW - 2 - fb) / 2, lw, label,
                         it->src == SRC_INFO && !focused ? C_MUTED : focused ? C_WHITE : C_TEXT);
        }
        if (sw >= 0) {
            int rx = ITEM_X + ITEM_W - 8;
            draw_switch(rx, y + (ROW - 2) / 2, focused && S.knob_id == S.cat * 256 + i ? S.knob.x : sw * 256, focused);
            if (note)
                gfx_text_right(FONT_SMALL, rx - SW_W - 6, y + (ROW - 2 - fs) / 2 + 1, note, focused ? C_WHITE : C_MUTED);
            continue;
        }
        char shown[64];
        int adjustable = it->src != SRC_INFO && it->src != SRC_ACTION && item_ready(it);
        snprintf(shown, sizeof(shown), focused && adjustable ? "< %s >" : "%s", v);
        gfx_text_right(FONT_SMALL, ITEM_X + ITEM_W - 8, y + (ROW - 2 - fs) / 2 + 1, shown,
                       focused ? C_WHITE : it->src == SRC_ACTION ? C_FOCUS : C_MUTED);
    }
    if (!c->n)
        gfx_text(FONT_SMALL, ITEM_X + 8, TOP_Y + 4, S.loaded ? "Nothing here" : "...", C_MUTED);
    /* scroll marks */
    if (top > 0)
        gfx_text_right(FONT_SMALL, SCREEN_W - 4, TOP_Y - 14, "^", C_MUTED);
    if (top + VISIBLE < c->n)
        gfx_text_right(FONT_SMALL, SCREEN_W - 4, TOP_Y + VISIBLE * ROW - 10, "v", C_MUTED);

    /* description of what is focused */
    gfx_rect(CAT_X, DESC_Y - 4, SCREEN_W - 2 * CAT_X, 1, C_LINE);
    const char *desc = c->note;
    char pdesc[PLUG_PATH + 96];
    if (S.col == 1 && c->n) {
        Item *it = &c->items[sel];
        if (it->src == SRC_PLUG) { /* what the choice means, then the file */
            int v = S.plug_v[it->key];
            snprintf(pdesc, sizeof(pdesc), "%s %s", v >= 0 ? PRL_DESC[v] : "Custom runlevel.",
                     S.plug[it->key].path);
            desc = pdesc;
        } else if (it->desc) {
            desc = it->desc;
        }
    }
    if (desc)
        draw_wrapped(CAT_X + 4, DESC_Y, SCREEN_W - 2 * CAT_X - 8, desc, C_MUTED);

    /* footer */
    gfx_rect(0, FOOTER_Y, SCREEN_W, 1, C_LINE);
    static const Hint cat_hints[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Open"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
        {-1, "L/R", 0, "Section"},
    };
    static const Hint item_hints[] = {
        {-1, "<-/->", 0, "Change"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
        {-1, "L/R", 0, "Section"},
    };
    static const Hint switch_hints[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Switch"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
        {-1, "L/R", 0, "Section"},
    };
    static const Hint action_hints[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Select"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
        {-1, "L/R", 0, "Section"},
    };
    const Hint *h = cat_hints;
    int n = COUNT(cat_hints);
    if (S.col == 1 && c->n) {
        int src = c->items[sel].src;
        if (src == SRC_ACTION) { h = action_hints; n = COUNT(action_hints); }
        else if (switch_state(&c->items[sel]) >= 0) { h = switch_hints; n = COUNT(switch_hints); }
        else if (src != SRC_INFO) { h = item_hints; n = COUNT(item_hints); }
        else { h = item_hints + 1; n = 2; }
    }
    ui_draw_hints(h, n, SCREEN_W / 2, FOOTER_Y + 5);
    draw_space(t);
    gfx_set_opacity(256);
}
