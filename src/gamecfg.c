/* XLT - per-game settings, favorites and xlt.ini. Small text files, so
 * they stay readable and editable by hand. */
#include <pspiofilemgr.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "gamecfg.h"
#include "launch.h"
#include "groups.h"
#include "theme.h"
#include "bg.h"
#include "musicsort.h"

#define FAV_MAX 64
#define HIDDEN_MAX 256

GlobalCfg g_cfg = {-1, DRIVER_DEFAULT, {1, 2, 3}, 2, 0, 1, 0, 0, {0, 0}, 1, "", 1, ""}; /* Games, Plugins, Power; screensaver after 2 min; songs by artist; games by name; next video on */

static char s_dir[200];
static const char *const LEVEL_KEYS[3] = {"off", "song", "album"}; /* LEVEL_* in audiofmt.h */

static int read_file(const char *path, char *buf, int cap)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return -1;
    int n = sceIoRead(fd, buf, cap - 1);
    sceIoClose(fd);
    buf[n > 0 ? n : 0] = 0;
    return n;
}

static void write_file(const char *path, const char *buf, int len)
{
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return;
    sceIoWrite(fd, buf, len);
    sceIoClose(fd);
}

/* Finds "key=value" in an ini-style text; copies the value. */
static int ini_get(const char *text, const char *key, char *out, int n)
{
    size_t kl = strlen(key);
    for (const char *l = text; *l;) {
        while (*l == ' ' || *l == '\t') l++;
        if (!strncasecmp(l, key, kl) && l[kl] == '=') {
            const char *v = l + kl + 1;
            int i = 0;
            while (v[i] && v[i] != '\n' && v[i] != '\r' && i < n - 1) { out[i] = v[i]; i++; }
            out[i] = 0;
            return 1;
        }
        const char *nl = strchr(l, '\n');
        if (!nl) break;
        l = nl + 1;
    }
    return 0;
}

static void cfg_path(const Entry *e, char *out, int n)
{
    int valid = strlen(e->id) == 9;
    for (int i = 0; valid && i < 9; i++)
        valid = isalnum((unsigned char)e->id[i]);
    if (valid)
        snprintf(out, n, "%s/games/%s.cfg", s_dir, e->id);
    else
        snprintf(out, n, "%s/games/%08X.cfg", s_dir, (unsigned)e->key);
}

void gamecfg_forget(const Entry *e)
{
    char path[256];
    cfg_path(e, path, sizeof(path));
    sceIoRemove(path);
}

static const char *driver_name(int d)
{
    return d == DRIVER_INFERNO ? "inferno" : d == DRIVER_MARCH33 ? "march33" : "np9660";
}

void gamecfg_load(const Entry *e, GameCfg *out)
{
    char path[260], text[256], v[32];
    out->driver = g_cfg.iso_driver;
    cfg_path(e, path, sizeof(path));
    if (read_file(path, text, sizeof(text)) <= 0)
        return;
    if (ini_get(text, "driver", v, sizeof(v))) {
        if (!strcasecmp(v, "inferno")) out->driver = DRIVER_INFERNO;
        else if (!strcasecmp(v, "march33")) out->driver = DRIVER_MARCH33;
        else if (!strcasecmp(v, "np9660")) out->driver = DRIVER_NP9660;
    }
}

void gamecfg_save(const Entry *e, const GameCfg *cfg)
{
    char path[260], text[128];
    cfg_path(e, path, sizeof(path));
    if (cfg->driver == g_cfg.iso_driver) {
        sceIoRemove(path); /* the same as the default: nothing to remember */
        return;
    }
    int n = snprintf(text, sizeof(text), "# %s\ndriver=%s\n", e->title, driver_name(cfg->driver));
    write_file(path, text, n < (int)sizeof(text) ? n : (int)sizeof(text) - 1);
}

/* ------------------------------------------------- favorites, hidden */

#define PATH_LEN sizeof(((Entry *)0)->path)

typedef struct {
    const char *file;   /* name inside cache/ */
    int max, n;
    char (*items)[PATH_LEN];
} PathSet;

static char s_fav_items[FAV_MAX][PATH_LEN];
static char s_hidden_items[HIDDEN_MAX][PATH_LEN];
static PathSet s_fav = {"favorites.txt", FAV_MAX, 0, s_fav_items};
static PathSet s_hidden = {"hidden.txt", HIDDEN_MAX, 0, s_hidden_items};

static void set_save(const PathSet *ps)
{
    static char buf[HIDDEN_MAX * (PATH_LEN + 1)];
    int len = 0;
    for (int i = 0; i < ps->n && len < (int)sizeof(buf); i++)
        len += snprintf(buf + len, sizeof(buf) - len, "%s\n", ps->items[i]);
    if (len > (int)sizeof(buf)) len = sizeof(buf);
    char path[260];
    snprintf(path, sizeof(path), "%s/%s", s_dir, ps->file);
    write_file(path, buf, len);
}

static void set_load(PathSet *ps)
{
    static char buf[HIDDEN_MAX * (PATH_LEN + 1) + 1];
    char path[260];
    snprintf(path, sizeof(path), "%s/%s", s_dir, ps->file);
    ps->n = 0;
    if (read_file(path, buf, sizeof(buf)) <= 0)
        return;
    char *save = NULL;
    for (char *l = strtok_r(buf, "\r\n", &save); l && ps->n < ps->max; l = strtok_r(NULL, "\r\n", &save))
        if (*l)
            snprintf(ps->items[ps->n++], PATH_LEN, "%s", l);
}

static int set_has(const PathSet *ps, const char *path)
{
    for (int i = 0; i < ps->n; i++)
        if (!strcmp(ps->items[i], path))
            return 1;
    return 0;
}

static int set_put(PathSet *ps, const char *path, int on)
{
    int i = 0;
    while (i < ps->n && strcmp(ps->items[i], path))
        i++;
    if (on) {
        if (i < ps->n) return 0;
        if (ps->n == ps->max) return -1;
        snprintf(ps->items[ps->n++], PATH_LEN, "%s", path);
    } else {
        if (i == ps->n) return 0;
        memmove(&ps->items[i], &ps->items[i + 1], (size_t)(ps->n - i - 1) * PATH_LEN);
        ps->n--;
    }
    set_save(ps);
    return 0;
}

int favorite_get(const char *path) { return set_has(&s_fav, path); }
void favorite_set(const char *path, int on) { set_put(&s_fav, path, on); }
int hidden_get(const char *path) { return set_has(&s_hidden, path); }
int hidden_set(const char *path, int on) { return set_put(&s_hidden, path, on); }

/* ------------------------------------------------------------------ init */

void globalcfg_save(void)
{
    char path[260], text[512];
    snprintf(path, sizeof(path), "%s/xlt.ini", s_dir);
    int n = snprintf(text, sizeof(text),
                     "brightness_pct=%d\niso_driver=%s\nsettings_recent=%d,%d,%d\nsaver_minutes=%d\ngrid_views=%d\n"
                     "ui_sounds=%d\ntheme=%s\nmusic_sort=%s\ngames_sort=%s\nhomebrew_sort=%s\nautoplay_next=%d\naudio_lang=%s\n"
                     "subtitle_lang=%s\nvolume_level=%s\n",
                     g_cfg.brightness, driver_name(g_cfg.iso_driver), g_cfg.set_recent[0], g_cfg.set_recent[1],
                     g_cfg.set_recent[2], g_cfg.saver_min, g_cfg.grid, g_cfg.ui_sounds,
                     g_themes[g_cfg.theme >= 0 && g_cfg.theme < THEME_COUNT ? g_cfg.theme : 0].name,
                     msort_key(g_cfg.music_sort), gsort_key(g_cfg.game_sort[0]), gsort_key(g_cfg.game_sort[1]), g_cfg.autoplay, g_cfg.audio_lang,
                     g_cfg.sub_lang, LEVEL_KEYS[g_cfg.level >= 0 && g_cfg.level < 3 ? g_cfg.level : 1]);
    write_file(path, text, n);
}

/* cache/groups.txt: Games / Homebrew tabs and folders (groups.c). */
typedef struct { char *text; int len; char path[220]; } GroupsArg;

static void job_save_groups(void *arg)
{
    const GroupsArg *a = arg;
    write_file(a->path, a->text, a->len);
    free(a->text);
}

void groups_save(void)
{
    GroupsArg a;
    int cap = groups_serialized_max();
    a.text = malloc(cap);
    if (!a.text)
        return;
    a.len = groups_serialize(a.text, cap);
    if (a.len < 0) {
        free(a.text);
        return;
    }
    snprintf(a.path, sizeof(a.path), "%s/groups.txt", s_dir);
    bg_post(job_save_groups, &a, sizeof(a)); /* the worker writes, then frees the text */
}

static void groups_load(void)
{
    char path[260];
    int cap = groups_serialized_max();
    char *text = malloc(cap);
    if (!text)
        return;
    snprintf(path, sizeof(path), "%s/groups.txt", s_dir);
    if (read_file(path, text, cap) > 0)
        groups_parse(text);
    free(text);
}

void gamecfg_init(const char *app_dir)
{
    snprintf(s_dir, sizeof(s_dir), "%.180scache", app_dir);
    char path[260];
    snprintf(path, sizeof(path), "%s/games", s_dir);
    sceIoMkdir(s_dir, 0777);
    sceIoMkdir(path, 0777);

    set_load(&s_fav);
    set_load(&s_hidden);
    groups_load();

    char text[512], v[16];
    snprintf(path, sizeof(path), "%s/xlt.ini", s_dir);
    int got = read_file(path, text, sizeof(text));
    if (got <= 0) { /* before the rename to XLT the file was xlite.ini: read it once, xlt.ini is written next */
        snprintf(path, sizeof(path), "%s/xlite.ini", s_dir);
        got = read_file(path, text, sizeof(text));
    }
    if (got > 0) {
        if (ini_get(text, "brightness_pct", v, sizeof(v))) {
            int b = atoi(v); /* the old 4-step "brightness=" key is ignored */
            g_cfg.brightness = b >= 0 && b <= 100 ? b : -1;
        }
        if (ini_get(text, "settings_recent", v, sizeof(v))) {
            int r[3], k = sscanf(v, "%d,%d,%d", &r[0], &r[1], &r[2]);
            for (int i = 0; i < k && i < 3; i++)
                if (r[i] >= 0 && r[i] < 8)
                    g_cfg.set_recent[i] = r[i];
        }
        if (ini_get(text, "saver_minutes", v, sizeof(v))) {
            int m = atoi(v);
            g_cfg.saver_min = m >= 0 && m <= 60 ? m : 2;
        }
        if (ini_get(text, "theme", v, sizeof(v)))
            for (int i = 0; i < THEME_COUNT; i++)
                if (!strcasecmp(v, g_themes[i].name))
                    g_cfg.theme = i;
        if (ini_get(text, "ui_sounds", v, sizeof(v)))
            g_cfg.ui_sounds = atoi(v) != 0;
        if (ini_get(text, "autoplay_next", v, sizeof(v)))
            g_cfg.autoplay = atoi(v) != 0;
        if (ini_get(text, "audio_lang", v, sizeof(v)) && strlen(v) == 3)
            snprintf(g_cfg.audio_lang, sizeof(g_cfg.audio_lang), "%s", v);
        if (ini_get(text, "volume_level", v, sizeof(v)))
            for (int i = 0; i < 3; i++)
                if (!strcmp(v, LEVEL_KEYS[i])) g_cfg.level = i;
        if (ini_get(text, "subtitle_lang", v, sizeof(v)) && strlen(v) == 3)
            snprintf(g_cfg.sub_lang, sizeof(g_cfg.sub_lang), "%s", v);
        if (ini_get(text, "music_sort", v, sizeof(v)))
            g_cfg.music_sort = msort_parse(v);
        if (ini_get(text, "games_sort", v, sizeof(v)))
            g_cfg.game_sort[0] = gsort_parse(v);
        if (ini_get(text, "homebrew_sort", v, sizeof(v)))
            g_cfg.game_sort[1] = gsort_parse(v);
        if (ini_get(text, "grid_views", v, sizeof(v)))
            g_cfg.grid = atoi(v) & 0xFF;
        if (ini_get(text, "iso_driver", v, sizeof(v))) {
            if (!strcasecmp(v, "inferno")) g_cfg.iso_driver = DRIVER_INFERNO;
            else if (!strcasecmp(v, "march33")) g_cfg.iso_driver = DRIVER_MARCH33;
            else g_cfg.iso_driver = DRIVER_NP9660;
        }
    }
}
