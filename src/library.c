/* XLT - library index and scanner.
 *
 * Music (MP3, M4A, AAC, FLAC in MUSIC/ and two levels of subfolders) is read for its
 * tags only; the cover art is located, not loaded.
 * Pictures (JPEG/PNG/BMP in PICTURE/ and one level of subfolders) are
 * listed too; they are never opened by the scan, only stat'ed.
 *
 * Boot: read <app>/cache/index.bin (fixed-size Entry records) and show it at
 * once. Then a background thread walks the game folders in delpath.c (ISO/
 * and PSP/GAME/, with their subfolders) plus the media folders on ms0: and ef0:,
 * reusing every cached entry whose path, size and mtime are unchanged, and
 * only opening files that are new or changed. If the result differs from
 * the cache, it rewrites index.bin and hands the new list to the main thread.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "groups.h"
#include "library.h"
#include "system.h"
#include "history.h"
#include "gamecfg.h"
#include "safestart.h"
#include "audiofmt.h"
#include "imgdecode.h"
#include "id3.h"
#include "mp4.h"
#include "delpath.h"
#include "musicsort.h"

#define RECENT_MAX 3

/* Owned by the main thread. */
static Entry *s_entries;
static int s_count;
static int *s_view[VIEW_COUNT];
static int s_view_n[VIEW_COUNT];
static int s_recent[VIEW_COUNT][RECENT_MAX];
static int s_recent_n[VIEW_COUNT];

/* Handoff from the scanner thread. */
static volatile int s_scanning;
static volatile int s_ready;
static Entry *volatile s_result;
static volatile int s_result_n;
static volatile int s_stop;
static int s_rescan_after; /* an entry was deleted during a scan: its result is stale */
static SceUID s_thread = -1;

static char s_index_path[256];
static char s_log_path[256];

/* Progress of the running scan, for the boot screen (written by the scan thread). */
static volatile int s_prog_done, s_prog_total, s_prog_phase;
static int s_opened; /* files that had to be read (new or changed) */
static char s_self[256];

/* --------------------------------------------------------------- index io */

static int load_index(Entry **out)
{
    *out = NULL;
    SceUID fd = sceIoOpen(s_index_path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return 0;
    IndexHeader h;
    int n = 0;
    if (sceIoRead(fd, &h, sizeof(h)) == sizeof(h) && h.magic == INDEX_MAGIC &&
        h.version == INDEX_VERSION && h.entry_size == sizeof(Entry) && h.count < 10000) {
        Entry *e = malloc(sizeof(Entry) * (h.count ? h.count : 1));
        int bytes = (int)(sizeof(Entry) * h.count);
        if (e && sceIoRead(fd, e, bytes) == bytes) {
            for (uint32_t i = 0; i < h.count; i++) {
                e[i].path[sizeof(e[i].path) - 1] = 0;
                e[i].title[sizeof(e[i].title) - 1] = 0;
                e[i].id[sizeof(e[i].id) - 1] = 0;
                e[i].sysver[sizeof(e[i].sysver) - 1] = 0;
            }
            *out = e;
            n = (int)h.count;
        } else {
            free(e);
        }
    }
    sceIoClose(fd);
    return n;
}

static void save_index(const Entry *e, int n)
{
    SceUID fd = sceIoOpen(s_index_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return;
    IndexHeader h = {INDEX_MAGIC, INDEX_VERSION, (uint32_t)n, sizeof(Entry)};
    sceIoWrite(fd, &h, sizeof(h));
    if (n)
        sceIoWrite(fd, e, sizeof(Entry) * n);
    sceIoClose(fd);
}

/* ------------------------------------------------------------------ views */

/* Pictures: newest first, then by name. */
static int cmp_newest(const void *a, const void *b)
{
    const Entry *x = &s_entries[*(const int *)a], *y = &s_entries[*(const int *)b];
    if (x->mtime != y->mtime)
        return x->mtime > y->mtime ? -1 : 1;
    return strcasecmp(x->title, y->title);
}

/* Music: in the order picked in the song's options (musicsort.c, xlt.ini). */
static int cmp_music(const void *a, const void *b)
{
    return msort_cmp(&s_entries[*(const int *)a], &s_entries[*(const int *)b], g_cfg.music_sort);
}

/* Games / Homebrew: in the order picked for that tab (musicsort.c, xlt.ini). */
static int s_sort_view; /* the tab being sorted (qsort has no context argument) */
static int cmp_game(const void *a, const void *b)
{
    const Entry *x = &s_entries[*(const int *)a], *y = &s_entries[*(const int *)b];
    int mode = g_cfg.game_sort[s_sort_view == VIEW_HOMEBREW];
    uint64_t px = mode == GSORT_PLAYED ? history_played(x->path) : 0;
    uint64_t py = mode == GSORT_PLAYED ? history_played(y->path) : 0;
    return gsort_cmp(x, y, mode, px, py);
}

static int cmp_title(const void *a, const void *b)
{
    const Entry *x = &s_entries[*(const int *)a], *y = &s_entries[*(const int *)b];
    int r = strcasecmp(x->title, y->title);
    return r ? r : strcmp(x->path, y->path);
}

/* 1 if entry a belongs before entry b in the Recent row:
 * favorites first, then last played, then newest file. */
static int newer(int a, int b)
{
    int fa = favorite_get(s_entries[a].path), fb = favorite_get(s_entries[b].path);
    if (fa != fb)
        return fa > fb;
    uint64_t pa = history_played(s_entries[a].path), pb = history_played(s_entries[b].path);
    if (pa != pb)
        return pa > pb;
    return s_entries[a].mtime > s_entries[b].mtime;
}

static void build_views(void)
{
    for (int v = 0; v < VIEW_COUNT; v++) {
        free(s_view[v]);
        s_view[v] = malloc(sizeof(int) * (s_count ? s_count : 1));
        s_view_n[v] = 0;
        s_recent_n[v] = 0;
    }
    for (int i = 0; i < s_count; i++) {
        int k = s_entries[i].kind;
        int v = hidden_get(s_entries[i].path) ? VIEW_HIDDEN
              : kind_is_picture(k) ? VIEW_PICTURES
              : kind_is_music(k) ? VIEW_MUSIC
              : kind_is_video(k) ? VIEW_VIDEO
              : groups_tab(s_entries[i].path, kind_is_homebrew(k) ? VIEW_HOMEBREW : VIEW_GAMES);
        if (s_view[v])
            s_view[v][s_view_n[v]++] = i;
    }
    for (int v = 0; v < VIEW_COUNT; v++) {
        if (!s_view[v])
            continue;
        s_sort_view = v;
        qsort(s_view[v], s_view_n[v], sizeof(int),
              v == VIEW_PICTURES ? cmp_newest : v == VIEW_MUSIC ? cmp_music
              : v == VIEW_GAMES || v == VIEW_HOMEBREW ? cmp_game : cmp_title);
        /* Recent: tiny top-N, last played first, then newest files. */
        for (int k = 0; k < s_view_n[v]; k++) {
            int idx = s_view[v][k];
            int pos = s_recent_n[v];
            while (pos > 0 && newer(idx, s_recent[v][pos - 1]))
                pos--;
            if (pos >= RECENT_MAX)
                continue;
            int last = s_recent_n[v] < RECENT_MAX ? s_recent_n[v] : RECENT_MAX - 1;
            for (int j = last; j > pos; j--)
                s_recent[v][j] = s_recent[v][j - 1];
            s_recent[v][pos] = idx;
            if (s_recent_n[v] < RECENT_MAX)
                s_recent_n[v]++;
        }
    }
}

int library_count(int v) { return s_view_n[v]; }
const Entry *library_get(int v, int i) { return &s_entries[s_view[v][i]]; }
int library_recent_count(int v) { return s_recent_n[v]; }
const Entry *library_recent(int v, int i) { return &s_entries[s_recent[v][i]]; }
int library_scanning(void) { return s_scanning; }
const char *library_self(void) { return s_self; }

int library_find(int v, const char *path)
{
    for (int i = 0; i < s_view_n[v]; i++)
        if (!strcmp(s_entries[s_view[v][i]].path, path))
            return i;
    return -1;
}

/* ---------------------------------------------------------------- scanner */

typedef struct {
    Entry *old;          /* cached entries, sorted by path */
    int old_n;
    Entry *out;
    int n, cap;
} Scan;

static int cmp_path(const void *a, const void *b)
{
    return strcmp(((const Entry *)a)->path, ((const Entry *)b)->path);
}

static uint32_t pack_time(const ScePspDateTime *t)
{
    return (uint32_t)((t->year - 1980) & 0x7F) << 25 | (uint32_t)t->month << 21 | (uint32_t)t->day << 16 |
           (uint32_t)t->hour << 11 | (uint32_t)t->minute << 5 | (uint32_t)(t->second >> 1);
}

enum { ADD_GAME, ADD_PICTURE, ADD_MUSIC, ADD_VIDEO };

/* "12:34" or "1:23:45" into the 8-byte sysver field (longer lengths are cut). */
static void length_str(char *b, int n, uint32_t ms)
{
    unsigned s = (unsigned)(ms / 1000);
    char tmp[24];
    if (s >= 3600) snprintf(tmp, sizeof(tmp), "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    else snprintf(tmp, sizeof(tmp), "%u:%02u", s / 60, s % 60);
    snprintf(b, n, "%.7s", tmp);
}

/* cover.jpg / folder.jpg (or .png) in the folder of `path`, checked once per folder. */
static int folder_has_art(const char *path)
{
    static char last_dir[200];
    static int last_result;
    const char *slash = strrchr(path, '/');
    int n = slash ? (int)(slash - path) : 0;
    if (n < (int)sizeof(last_dir) && !strncmp(last_dir, path, n) && last_dir[n] == 0)
        return last_result;
    snprintf(last_dir, sizeof(last_dir), "%.*s", n, path);
    static const char *names[] = {"cover.jpg", "folder.jpg", "cover.png", "folder.png"};
    char p[256];
    SceIoStat st;
    last_result = 0;
    for (int i = 0; i < 4 && !last_result; i++) {
        snprintf(p, sizeof(p), "%s/%s", last_dir, names[i]);
        last_result = sceIoGetstat(p, &st) >= 0;
    }
    return last_result;
}

static void add_file(Scan *s, const char *path, const SceIoStat *st, int what)
{
    if (!strcasecmp(path, s_self) || strlen(path) >= sizeof(((Entry *)0)->path))
        return;
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 64;
        Entry *p = realloc(s->out, sizeof(Entry) * cap);
        if (!p)
            return;
        s->out = p;
        s->cap = cap;
    }
    Entry *e = &s->out[s->n];
    memset(e, 0, sizeof(*e));
    strcpy(e->path, path);
    e->size = (uint64_t)st->st_size;
    e->mtime = pack_time(&st->sce_st_mtime);

    s_prog_done++;
    Entry *hit = s->old_n ? bsearch(e, s->old, s->old_n, sizeof(Entry), cmp_path) : NULL;
    if (!(hit && hit->size == e->size && hit->mtime == e->mtime))
        s_opened++;
    if (hit && hit->size == e->size && hit->mtime == e->mtime) {
        *e = *hit; /* unchanged: no need to open the file */
        s->n++;
        return;
    }
    if (what == ADD_VIDEO) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        const char *dot = strrchr(base, '.');
        snprintf(e->title, sizeof(e->title), "%.*s", dot ? (int)(dot - base) : (int)strlen(base), base);
        e->kind = KIND_VIDEO;
        if (dot && !strcasecmp(dot, ".pmf")) {
            uint8_t h[144];
            SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
            if (fd >= 0) {
                if (sceIoRead(fd, h, sizeof(h)) == sizeof(h) && !memcmp(h, "PSMF", 4))
                    snprintf(e->id, sizeof(e->id), "%dx%d", h[142] * 16, h[143] * 16);
                sceIoClose(fd);
            }
        } else {
            static Mp4 m; /* only the size and length are kept; the tables are freed again */
            if (mp4_open(&m, path, NULL) == 0) {
                snprintf(e->id, sizeof(e->id), "%dx%d", m.width, m.height);
                length_str(e->sysver, sizeof(e->sysver), m.duration_ms);
                mp4_close(&m);
            }
        }
        e->key = entry_key(e);
        s->n++;
        return;
    }
    if (what == ADD_MUSIC) {
        Id3Info t;
        if (audio_tags(path, e->size, &t) != 0)
            return;
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (t.title[0]) {
            snprintf(e->title, sizeof(e->title), "%s", t.title);
        } else { /* the file name without its extension */
            const char *dot = strrchr(base, '.');
            snprintf(e->title, sizeof(e->title), "%.*s", dot ? (int)(dot - base) : (int)strlen(base), base);
        }
        snprintf(e->artist, sizeof(e->artist), "%s", t.artist);
        snprintf(e->album, sizeof(e->album), "%s", t.album);
        e->track = t.track > 0 && t.track < 65536 ? t.track : 0;
        e->kind = KIND_MUSIC;
        if (t.art_size) {
            e->icon_off = t.art_off;
            e->icon_size = t.art_size;
        } else if (folder_has_art(path)) {
            e->icon_off = ICON_FOLDER_ART;
            e->icon_size = 1;
        }
        e->key = entry_key(e);
        s->n++;
        return;
    }
    if (what == ADD_PICTURE) {
        /* No need to open it: the name is the title, the thumbnail comes later. */
        const char *base = strrchr(path, '/');
        snprintf(e->title, sizeof(e->title), "%s", base ? base + 1 : path);
        e->kind = KIND_PICTURE;
        e->icon_size = 1;
        e->key = entry_key(e);
        s->n++;
        return;
    }
    if (probe_entry(e) == 0)
        s->n++;
}

static int has_ext(const char *name, const char *ext);

static void scan_music_dir(Scan *s, const char *dir, int depth)
{
    SceUID d = sceIoDopen(dir);
    if (d < 0)
        return;
    SceIoDirent de;
    char path[256];
    memset(&de, 0, sizeof(de));
    while (!s_stop && sceIoDread(d, &de) > 0) {
        const char *name = de.d_name;
        if (name[0] != '.' && snprintf(path, sizeof(path), "%s/%s", dir, name) < (int)sizeof(path)) {
            if (FIO_S_ISDIR(de.d_stat.st_mode)) {
                if (depth < 2) /* MUSIC/Artist/Album */
                    scan_music_dir(s, path, depth + 1);
            } else if (audio_format(name) != AUDIO_NONE) {
                add_file(s, path, &de.d_stat, ADD_MUSIC);
            }
        }
        memset(&de, 0, sizeof(de));
    }
    sceIoDclose(d);
}

static void scan_video_dir(Scan *s, const char *dir, int depth)
{
    SceUID d = sceIoDopen(dir);
    if (d < 0)
        return;
    SceIoDirent de;
    char path[256];
    memset(&de, 0, sizeof(de));
    while (!s_stop && sceIoDread(d, &de) > 0) {
        const char *name = de.d_name;
        if (name[0] != '.' && snprintf(path, sizeof(path), "%s/%s", dir, name) < (int)sizeof(path)) {
            if (FIO_S_ISDIR(de.d_stat.st_mode)) {
                if (depth < 1)
                    scan_video_dir(s, path, depth + 1);
            } else if (has_ext(name, ".mp4") || has_ext(name, ".m4v") || has_ext(name, ".pmf")) {
                add_file(s, path, &de.d_stat, ADD_VIDEO);
            }
        }
        memset(&de, 0, sizeof(de));
    }
    sceIoDclose(d);
}

static void scan_picture_dir(Scan *s, const char *dir, int depth)
{
    SceUID d = sceIoDopen(dir);
    if (d < 0)
        return;
    SceIoDirent de;
    char path[256];
    memset(&de, 0, sizeof(de));
    while (!s_stop && sceIoDread(d, &de) > 0) {
        const char *name = de.d_name;
        if (name[0] != '.' && snprintf(path, sizeof(path), "%s/%s", dir, name) < (int)sizeof(path)) {
            if (FIO_S_ISDIR(de.d_stat.st_mode)) {
                if (depth < 1)
                    scan_picture_dir(s, path, depth + 1);
            } else if (img_is_picture(name)) {
                add_file(s, path, &de.d_stat, ADD_PICTURE);
            }
        }
        memset(&de, 0, sizeof(de));
    }
    sceIoDclose(d);
}

static int has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), m = strlen(ext);
    return n > m && !strcasecmp(name + n - m, ext);
}

static void scan_iso_dir(Scan *s, const char *dir, int depth, int max_depth)
{
    SceUID d = sceIoDopen(dir);
    if (d < 0)
        return;
    SceIoDirent de;
    char path[256];
    memset(&de, 0, sizeof(de));
    while (!s_stop && sceIoDread(d, &de) > 0) {
        const char *name = de.d_name;
        if (name[0] != '.' && snprintf(path, sizeof(path), "%s/%s", dir, name) < (int)sizeof(path)) {
            if (FIO_S_ISDIR(de.d_stat.st_mode)) {
                if (depth < max_depth && strcasecmp(name, "VIDEO"))
                    scan_iso_dir(s, path, depth + 1, max_depth);
            } else if (has_ext(name, ".iso") || has_ext(name, ".cso") || has_ext(name, ".zso")) {
                add_file(s, path, &de.d_stat, ADD_GAME);
            }
        }
        memset(&de, 0, sizeof(de));
    }
    sceIoDclose(d);
}

/* Folders with an EBOOT.PBP are entries; the others (category folders) are
 * searched the same way, up to max_depth levels down. */
static void scan_game_dir(Scan *s, const char *dir, int depth, int max_depth)
{
    SceUID d = sceIoDopen(dir);
    if (d < 0)
        return;
    SceIoDirent de;
    SceIoStat st;
    char path[256];
    memset(&de, 0, sizeof(de));
    while (!s_stop && sceIoDread(d, &de) > 0) {
        const char *name = de.d_name;
        /* skip dot entries and old kxploit "%" folders */
        if (name[0] != '.' && name[0] != '%' && FIO_S_ISDIR(de.d_stat.st_mode) &&
            snprintf(path, sizeof(path), "%s/%s/EBOOT.PBP", dir, name) < (int)sizeof(path)) {
            memset(&st, 0, sizeof(st));
            if (sceIoGetstat(path, &st) >= 0) {
                add_file(s, path, &st, ADD_GAME);
            } else if (depth < max_depth) {
                path[strlen(path) - strlen("/EBOOT.PBP")] = 0;
                scan_game_dir(s, path, depth + 1, max_depth);
            }
        }
        memset(&de, 0, sizeof(de));
    }
    sceIoDclose(d);
}

static int same_list(const Entry *a, int an, const Entry *b, int bn)
{
    return an == bn && (an == 0 || !memcmp(a, b, sizeof(Entry) * an));
}

/* One line per scan in cache/scan.log (a fresh log each start). */
static void scan_log(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    SceUID fd = sceIoOpen(s_log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, line, n);
        sceIoClose(fd);
    }
}

static int ms_since(SceInt64 t0) { return (int)((sceKernelGetSystemTimeWide() - t0) / 1000); }

static int scan_thread(SceSize args, void *argp)
{
    static const char *roots[] = {"ms0:", "ef0:"};
    Scan s = {0};
    char dir[96];
    int ms[LIB_PHASES] = {0};
    SceInt64 t0 = sceKernelGetSystemTimeWide(), tp;

    power_boost();
    s.old_n = load_index(&s.old);
    if (s.old_n)
        qsort(s.old, s.old_n, sizeof(Entry), cmp_path);
    s_prog_total = s.old_n; /* what was there last time: the best guess of what's there now */
    s_prog_done = 0;
    s_opened = 0;
    int load_ms = ms_since(t0);

    for (int r = 0; r < 2 && !s_stop; r++) {
        s_prog_phase = LIB_PHASE_GAMES;
        tp = sceKernelGetSystemTimeWide();
        for (int i = 0; i < g_scan_dir_count && !s_stop; i++) {
            const ScanDir *sd = &g_scan_dirs[i];
            snprintf(dir, sizeof(dir), "%s/%s", roots[r], sd->dir);
            if (sd->type == SCAN_DISC)
                scan_iso_dir(&s, dir, 0, sd->depth);
            else
                scan_game_dir(&s, dir, 0, sd->depth);
        }
        ms[LIB_PHASE_GAMES] += ms_since(tp);
        s_prog_phase = LIB_PHASE_PICTURES;
        tp = sceKernelGetSystemTimeWide();
        snprintf(dir, sizeof(dir), "%s/PICTURE", roots[r]);
        scan_picture_dir(&s, dir, 0);
        ms[LIB_PHASE_PICTURES] += ms_since(tp);
        s_prog_phase = LIB_PHASE_MUSIC;
        tp = sceKernelGetSystemTimeWide();
        snprintf(dir, sizeof(dir), "%s/MUSIC", roots[r]);
        scan_music_dir(&s, dir, 0);
        ms[LIB_PHASE_MUSIC] += ms_since(tp);
        s_prog_phase = LIB_PHASE_VIDEO;
        tp = sceKernelGetSystemTimeWide();
        snprintf(dir, sizeof(dir), "%s/VIDEO", roots[r]);
        scan_video_dir(&s, dir, 0);
        ms[LIB_PHASE_VIDEO] += ms_since(tp);
    }

    if (!s_stop) {
        qsort(s.out, s.n, sizeof(Entry), cmp_path);
        if (!same_list(s.out, s.n, s.old, s.old_n)) {
            save_index(s.out, s.n);
            s_result = s.out;
            s_result_n = s.n;
            s.out = NULL;
            s_ready = 1;
        }
    }
    int changed = s_ready;
    free(s.out);
    free(s.old);
    if (!s_stop)
        scan_log("scan %d ms: index %d, games %d, pictures %d, music %d, video %d; %d files (%d last time), "
                 "%d read, %s",
                 ms_since(t0), load_ms, ms[0], ms[1], ms[2], ms[3], s_prog_done, s_prog_total, s_opened,
                 changed ? "library changed" : "nothing changed");
    power_idle();
    s_scanning = 0;
    return 0;
}

int library_progress(int *done, int *total)
{
    if (done) *done = s_prog_done;
    if (total) *total = s_prog_total;
    return s_prog_phase;
}

void library_note(const char *msg) { scan_log("%s", msg); }

/* ------------------------------------------------------------------ setup */

void library_init(const char *app_dir, const char *self_path, int scan)
{
    char cache[200];
    snprintf(cache, sizeof(cache), "%.180scache", app_dir);
    sceIoMkdir(cache, 0777);
    snprintf(s_index_path, sizeof(s_index_path), "%s/index.bin", cache);
    snprintf(s_log_path, sizeof(s_log_path), "%s/scan.log", cache);
    sceIoRemove(s_log_path); /* a fresh log each start */
    snprintf(s_self, sizeof(s_self), "%s", self_path);

    SceInt64 t0 = sceKernelGetSystemTimeWide();
    s_count = load_index(&s_entries);
    build_views();
    scan_log("start: index of %d entries loaded in %d ms; %s", s_count, ms_since(t0),
             scan ? "scanning" : "back from a game: no scan");

    if (scan)
        library_rescan();
}

void library_rescan(void)
{
    if (s_scanning || g_safe_start)
        return; /* already running, or a safe start: no scan */
    if (s_thread >= 0) { /* finished earlier: reap it */
        sceKernelWaitThreadEnd(s_thread, NULL);
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
    }
    s_stop = 0;
    s_scanning = 1;
    s_thread = sceKernelCreateThread("xl_scan", scan_thread, 0x30, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (s_thread < 0 || sceKernelStartThread(s_thread, 0, NULL) < 0) {
        if (s_thread >= 0)
            sceKernelDeleteThread(s_thread);
        s_thread = -1;
        s_scanning = 0;
    }
}

void library_forget(const char *path)
{
    for (int i = 0; i < s_count; i++)
        if (!strcmp(s_entries[i].path, path)) {
            memmove(&s_entries[i], &s_entries[i + 1], sizeof(Entry) * (s_count - i - 1));
            s_count--;
            break;
        }
    build_views();
    if (s_scanning)
        s_rescan_after = 1; /* that scan may still list it */
    else
        library_rescan(); /* rewrites index.bin without it */
}

int library_poll(void)
{
    if (s_rescan_after && !s_scanning) {
        s_rescan_after = 0;
        if (s_ready) {
            free(s_result);
            s_ready = 0;
        }
        library_rescan();
        return 0;
    }
    if (!s_ready)
        return 0;
    free(s_entries);
    s_entries = s_result;
    s_count = s_result_n;
    s_ready = 0;
    build_views();
    return 1;
}

void library_shutdown(void)
{
    if (s_thread >= 0) {
        s_stop = 1;
        SceUInt timeout = 3000000;
        sceKernelWaitThreadEnd(s_thread, &timeout);
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
    }
    s_scanning = 0;
}

void library_resort(void)
{
    build_views();
}
