/* XLT - file manager.
 *
 * A list of the folder on the left, the focused item on the right (a preview
 * for pictures, type, size, date; free space for a storage device). Folders
 * first, then files, by name with numbers in number order (or by date, size
 * or type). ✕ opens: a folder goes in; a picture opens the viewer over the
 * folder's pictures; a song plays the folder's songs from there; a video
 * plays; text and PDF open the reader; ISO/CSO/ZSO and EBOOT.PBP start;
 * anything else opens as text or hex. ○ goes up, □ marks, △ opens the menu
 * (copy, cut, paste, rename, delete, new folder, sort, folder size).
 *
 * Listing, previews, renames and new folders run on the background worker;
 * copy, move and delete on their own thread (fileops.c), so browsing goes
 * on while they work. Afterwards the library is rescanned, because the
 * files may have been games, music, videos or pictures.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspgu.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "safestart.h"
#include "audiofmt.h"
#include "zip.h"
#include "fm.h"
#include "sfx.h"
#include "ui_internal.h"
#include "anim.h"
#include "bg.h"
#include "fileops.h"
#include "imgdecode.h"
#include "library.h"
#include "menu.h"
#include "music.h"
#include "osk.h"
#include "preview.h"
#include "reader.h"
#include "thumbs.h"
#include "sysctl.h"
#include "video.h"
#include "viewer.h"
#include "gamecfg.h"

#define FM_MAX 2048
#define NAME_LEN 200
#define FOOTER_Y 248
#define LIST_X 12
#define LIST_W 290
#define LIST_Y 30
#define ROW_H 22
#define LIST_ROWS 9
#define PANEL_X 314
#define PANEL_W (SCREEN_W - 12 - PANEL_X)
#define PREV_W 152
#define PREV_H 86
/* Grid view: 4 x 3 cells in the list's place, the panel stays. */
#define GRID_COLS 4
#define GRID_ROWS 3
#define CELL_W 66
#define CELL_H 60
#define CELL_GAP 8
#define CELL_VGAP 6

enum { FT_DIR, FT_DEVICE, FT_PICTURE, FT_MUSIC, FT_VIDEO, FT_TEXT, FT_PDF, FT_DISC, FT_PBP, FT_ARCHIVE, FT_OTHER };
enum { SORT_NAME, SORT_DATE, SORT_SIZE, SORT_TYPE, SORT_COUNT };

typedef struct {
    char name[NAME_LEN];
    uint64_t size;
    uint32_t mtime;
    uint8_t type, marked;
} FEnt;

static FEnt s_ents[FM_MAX]; /* written by the list job while F.loading, read by the UI otherwise */
static char s_clip[FOP_MAX_ITEMS][FOP_PATH];
static char s_work[FOP_MAX_ITEMS][FOP_PATH];

static struct {
    int open;
    Spring t;
    char cwd[FOP_PATH]; /* "" = the list of storage devices */
    int n, truncated, skipped, loading, gen, err;
    int sel, top;
    Spring hl, scroll; /* rows, Q8 (grid: the focused row and the top row) */
    Spring gcol;       /* grid: the focused column, Q8 */
    int sort;
    char select_name[NAME_LEN];
    int confirm_delete;
    int clip_n, clip_cut;
    /* preview of the focused picture */
    int prev_wait, prev_for, prev_ok, prev_w, prev_h;
    char prev_path[FOP_PATH];
    /* folder size */
    char size_path[FOP_PATH];
    uint64_t size_bytes;
    int size_files, size_pending;
    /* devices */
    int has_ef0;
    uint64_t dev_free[2], dev_total[2];
    int dev_ok[2];
    int rescan_pending, reload_pending;
    char recent[FM_PLACES][FOP_PATH];
    char app_dir[FOP_PATH];
    char self_dir[FOP_PATH];
} F;

static uint8_t s_prev_rgba[PREV_W * PREV_H * 4] __attribute__((aligned(64)));

/* ------------------------------------------------------------- helpers */

static void open_menu(void);

static const char *ext_of(const char *name)
{
    const char *d = strrchr(name, '.');
    return d && d != name ? d + 1 : "";
}

static int ext_in(const char *e, const char *list)
{
    int n = (int)strlen(e);
    if (!n)
        return 0;
    for (const char *p = list; *p;) {
        const char *q = strchr(p, ' ');
        int l = q ? (int)(q - p) : (int)strlen(p);
        if (l == n && !strncasecmp(p, e, n))
            return 1;
        if (!q) break;
        p = q + 1;
    }
    return 0;
}

static int ftype(const char *name, int dir)
{
    if (dir)
        return FT_DIR;
    const char *e = ext_of(name);
    if (img_is_picture(name)) return FT_PICTURE;
    if (audio_format(name) != AUDIO_NONE) return FT_MUSIC;
    if (ext_in(e, "mp4 m4v pmf")) return FT_VIDEO;
    if (ext_in(e, "pdf")) return FT_PDF;
    if (ext_in(e, "iso cso zso")) return FT_DISC;
    if (ext_in(e, "pbp")) return FT_PBP;
    if (ext_in(e, "zip rar 7z gz bz2 xz tar")) return FT_ARCHIVE;
    if (ext_in(e, "txt log ini cfg conf md csv tsv json xml htm html c h cpp hpp cc py lua js sh bat nfo diz "
                  "srt sub ass ssa yml yaml toml rtf tex me 1st url m3u pls cue"))
        return FT_TEXT;
    return FT_OTHER;
}

static int type_icon(int t)
{
    static const int icons[] = {ICON_FOLDER, ICON_STORAGE, ICON_PICTURE, ICON_MUSIC, ICON_VIDEO, ICON_DOC,
                                ICON_PDF, ICON_GAMES, ICON_HOMEBREW, ICON_FILE, ICON_FILE};
    return icons[t];
}

static u32 type_color(int t)
{
    static const u32 c[] = {0xd29922, 0x8b949e, 0x57ab5a, 0xe0a3e0, 0xa371f7, 0x8b949e,
                            0xe5534b, 0x58a6ff, 0x58a6ff, 0xc69026, 0x6e7681};
    return RGB(c[t]);
}

static const char *type_name(const FEnt *e)
{
    switch (e->type) {
    case FT_DIR: return "Folder";
    case FT_DEVICE: return "Storage";
    case FT_PICTURE: return !strcasecmp(ext_of(e->name), "png") ? "PNG picture"
                          : !strcasecmp(ext_of(e->name), "bmp") ? "BMP picture" : "JPEG picture";
    case FT_MUSIC: return audio_format_name(audio_format(e->name));
    case FT_VIDEO: return !strcasecmp(ext_of(e->name), "pmf") ? "PMF video" : "MP4 video";
    case FT_TEXT: return "Text";
    case FT_PDF: return "PDF document";
    case FT_DISC: return !strcasecmp(ext_of(e->name), "iso") ? "Disc image (ISO)" : "Compressed disc image";
    case FT_PBP: return "PSP application";
    case FT_ARCHIVE: return !strcasecmp(ext_of(e->name), "zip") ? "ZIP archive" : "Archive";
    default: return "File";
    }
}

static int join(char *out, const char *dir, const char *name)
{
    int n = (int)strlen(dir);
    return snprintf(out, FOP_PATH, n && dir[n - 1] == '/' ? "%s%s" : "%s/%s", dir, name) < FOP_PATH;
}

/* "ms0:/A/B" -> "ms0:/A", "ms0:/A" -> "ms0:/", "ms0:/" -> "" (the device list).
 * child gets the last part ("B"). */
static void parent_of(const char *p, char *out, char *child)
{
    int n = (int)strlen(p);
    while (n > 0 && p[n - 1] == '/') n--;
    int s = n;
    while (s > 0 && p[s - 1] != '/' && p[s - 1] != ':') s--;
    if (child)
        snprintf(child, NAME_LEN, "%.*s", n - s, p + s);
    if (s == 0 || p[s - 1] == ':') {
        out[0] = 0;
    } else if (s >= 2 && p[s - 2] == ':') {
        snprintf(out, FOP_PATH, "%.*s", s, p); /* the root keeps its slash */
    } else {
        snprintf(out, FOP_PATH, "%.*s", s - 1, p);
    }
}

static int is_root(const char *p) { return strlen(p) == 5 && p[3] == ':' && p[4] == '/'; }

static uint32_t pack_time(const ScePspDateTime *t)
{
    return (uint32_t)((t->year - 1980) & 0x7F) << 25 | (uint32_t)t->month << 21 | (uint32_t)t->day << 16 |
           (uint32_t)t->hour << 11 | (uint32_t)t->minute << 5 | (uint32_t)(t->second >> 1);
}

/* Names in number order: "IMG2" before "IMG10". Case is ignored. */
static int natcmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0' && isdigit((unsigned char)a[1])) a++;
            while (*b == '0' && isdigit((unsigned char)b[1])) b++;
            int la = 0, lb = 0;
            while (isdigit((unsigned char)a[la])) la++;
            while (isdigit((unsigned char)b[lb])) lb++;
            if (la != lb) return la - lb;
            int c = strncmp(a, b, la);
            if (c) return c;
            a += la;
            b += lb;
            continue;
        }
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int s_sort;
static int cmp_ent(const void *pa, const void *pb)
{
    const FEnt *a = pa, *b = pb;
    int ad = a->type == FT_DIR || a->type == FT_DEVICE, bd = b->type == FT_DIR || b->type == FT_DEVICE;
    if (ad != bd) return bd - ad;
    if (a->type == FT_DEVICE) return 0;
    int c = 0;
    switch (s_sort) {
    case SORT_DATE: c = a->mtime < b->mtime ? 1 : a->mtime > b->mtime ? -1 : 0; break; /* newest first */
    case SORT_SIZE: c = a->size < b->size ? 1 : a->size > b->size ? -1 : 0; break;     /* biggest first */
    case SORT_TYPE: c = strcasecmp(ext_of(a->name), ext_of(b->name)); break;
    }
    return c ? c : natcmp(a->name, b->name);
}

static const char *sort_name(int s)
{
    static const char *n[SORT_COUNT] = {"Name", "Date", "Size", "Type"};
    return n[s];
}

/* ------------------------------------------------------------- worker jobs */

typedef struct { int gen; char path[FOP_PATH]; } PathArg;

static volatile int s_list_ready;
static int s_list_gen, s_list_n, s_list_trunc, s_list_skipped, s_list_err, s_list_ef0;

static void job_list(void *arg)
{
    const PathArg *a = arg;
    int n = 0, trunc = 0, skipped = 0, err = 0;
    if (!a->path[0]) {
        memset(&s_ents[0], 0, sizeof(FEnt));
        snprintf(s_ents[0].name, NAME_LEN, "ms0:/");
        s_ents[0].type = FT_DEVICE;
        n = 1;
        SceUID d = sceIoDopen("ef0:/");
        s_list_ef0 = d >= 0;
        if (d >= 0) {
            sceIoDclose(d);
            memset(&s_ents[1], 0, sizeof(FEnt));
            snprintf(s_ents[1].name, NAME_LEN, "ef0:/");
            s_ents[1].type = FT_DEVICE;
            n = 2;
        }
    } else {
        SceUID d = sceIoDopen(a->path);
        if (d < 0) {
            err = 1;
        } else {
            SceIoDirent de;
            memset(&de, 0, sizeof(de));
            while (sceIoDread(d, &de) > 0) {
                const char *nm = de.d_name;
                if (strcmp(nm, ".") && strcmp(nm, "..")) {
                    if (n >= FM_MAX) {
                        trunc = 1;
                    } else if (strlen(nm) >= NAME_LEN || strlen(a->path) + strlen(nm) + 2 > FOP_PATH) {
                        skipped++;
                    } else {
                        FEnt *e = &s_ents[n++];
                        snprintf(e->name, NAME_LEN, "%s", nm);
                        int dir = FIO_S_ISDIR(de.d_stat.st_mode);
                        e->type = (uint8_t)ftype(nm, dir);
                        e->size = dir ? 0 : (uint64_t)de.d_stat.st_size;
                        e->mtime = pack_time(&de.d_stat.sce_st_mtime);
                        e->marked = 0;
                    }
                }
                memset(&de, 0, sizeof(de));
            }
            sceIoDclose(d);
        }
    }
    s_list_n = n;
    s_list_trunc = trunc;
    s_list_skipped = skipped;
    s_list_err = err;
    s_list_gen = a->gen;
    __sync_synchronize();
    s_list_ready = 1;
}

static volatile int s_dev_ready;
static uint64_t s_dev_free[2], s_dev_total[2];
static int s_dev_ok[2];

static volatile int s_ef0_probe = -1;

static void job_probe_ef0(void *arg)
{
    SceUID d = sceIoDopen("ef0:/");
    if (d >= 0)
        sceIoDclose(d);
    s_ef0_probe = d >= 0;
}

static void job_devinfo(void *arg)
{
    s_dev_ok[0] = sysctl_storage("ms0:", &s_dev_free[0], &s_dev_total[0]) == 0;
    s_dev_ok[1] = sysctl_storage("ef0:", &s_dev_free[1], &s_dev_total[1]) == 0;
    __sync_synchronize();
    s_dev_ready = 1;
}

typedef struct { int gen; char path[FOP_PATH]; } PrevArg;
static volatile int s_prev_ready;
static int s_prev_gen, s_prev_okv, s_prev_wv, s_prev_hv;

static void job_preview(void *arg)
{
    const PrevArg *a = arg;
    int w = 0, h = 0;
    power_boost();
    int ok = img_decode(a->path, IMG_FIT, PREV_W, PREV_H, s_prev_rgba, &w, &h) == 0;
    if (!library_scanning())
        power_idle();
    s_prev_okv = ok;
    s_prev_wv = w;
    s_prev_hv = h;
    s_prev_gen = a->gen;
    __sync_synchronize();
    s_prev_ready = 1;
}

typedef struct { int kind; char a[FOP_PATH], b[FOP_PATH]; } RenArg; /* kind 0 rename a->b, 1 mkdir a */
static volatile int s_ren_done, s_ren_err;

static void job_rename(void *arg)
{
    const RenArg *r = arg;
    int rc = r->kind ? sceIoMkdir(r->a, 0777) : sceIoRename(r->a, r->b);
    s_ren_err = rc < 0;
    __sync_synchronize();
    s_ren_done = 1;
}

typedef struct { char path[FOP_PATH]; } ProbeArg;
static volatile int s_probe_ready, s_probe_ok;
static Entry s_probe;

static void job_probe(void *arg)
{
    const ProbeArg *a = arg;
    Entry e;
    memset(&e, 0, sizeof(e));
    snprintf(e.path, sizeof(e.path), "%.*s", (int)sizeof(e.path) - 1, a->path); /* checked to fit before posting */
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    int ok = 0;
    if (sceIoGetstat(a->path, &st) >= 0) {
        e.size = (uint64_t)st.st_size;
        e.mtime = pack_time(&st.sce_st_mtime);
        ok = probe_entry(&e) == 0;
        e.key = entry_key(&e);
    }
    s_probe = e;
    s_probe_ok = ok;
    __sync_synchronize();
    s_probe_ready = 1;
}

static void job_save_recent(void *arg)
{
    char path[FOP_PATH + 16];
    snprintf(path, sizeof(path), "%scache/files.txt", F.app_dir);
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return;
    const char (*r)[FOP_PATH] = arg;
    for (int i = 0; i < FM_PLACES; i++)
        if (r[i][0]) {
            sceIoWrite(fd, r[i], strlen(r[i]));
            sceIoWrite(fd, "\n", 1);
        }
    sceIoClose(fd);
}

static char s_recent_load[FM_PLACES][FOP_PATH];
static volatile int s_recent_ready;

static void job_load_recent(void *arg)
{
    char path[FOP_PATH + 16], buf[FM_PLACES * FOP_PATH + 8];
    snprintf(path, sizeof(path), "%scache/files.txt", F.app_dir);
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd >= 0) {
        int n = sceIoRead(fd, buf, sizeof(buf) - 1);
        sceIoClose(fd);
        if (n > 0) {
            buf[n] = 0;
            int k = 0;
            for (char *l = buf; *l && k < FM_PLACES;) {
                char *nl = strchr(l, '\n');
                if (nl) *nl = 0;
                if ((!strncmp(l, "ms0:/", 5) || !strncmp(l, "ef0:/", 5)) && strlen(l) < FOP_PATH)
                    snprintf(s_recent_load[k++], FOP_PATH, "%s", l);
                if (!nl) break;
                l = nl + 1;
            }
        }
    }
    __sync_synchronize();
    s_recent_ready = 1;
}

/* ------------------------------------------------------------------ state */

void fm_init(const char *app_dir)
{
    snprintf(F.app_dir, sizeof(F.app_dir), "%s", app_dir);
    snprintf(F.self_dir, sizeof(F.self_dir), "%s", app_dir);
    int n = (int)strlen(F.self_dir);
    if (n && F.self_dir[n - 1] == '/') F.self_dir[n - 1] = 0;
    if (g_safe_start) { /* XLT's own folder, not safe/ inside it */
        char *s = strrchr(F.self_dir, '/');
        if (s) *s = 0;
    }
    fop_init();
    bg_post(job_load_recent, NULL, 0);
    bg_post(job_probe_ef0, NULL, 0); /* is there an ef0: (PSP Go)? Not the free space: that is slow */
}

static int sel_valid(void) { return !F.loading && F.n > 0 && F.sel >= 0 && F.sel < F.n; }
static FEnt *cur(void) { return sel_valid() ? &s_ents[F.sel] : NULL; }

static int is_grid(void) { return (g_cfg.grid & GRID_FILES) != 0; }

static void select_row(int i, int snap)
{
    if (i >= F.n) i = F.n - 1;
    if (i < 0) i = 0;
    if (i != F.sel) F.confirm_delete = 0;
    if (!snap && i != F.sel) sfx_play(SFX_MOVE);
    F.sel = i;
    if (is_grid()) { /* F.top counts rows here */
        int row = i / GRID_COLS, rows = (F.n + GRID_COLS - 1) / GRID_COLS;
        if (row < F.top) F.top = row;
        if (row >= F.top + GRID_ROWS) F.top = row - GRID_ROWS + 1;
        if (F.top > rows - GRID_ROWS) F.top = rows - GRID_ROWS;
        if (F.top < 0) F.top = 0;
        if (snap) {
            spring_snap(&F.hl, FX(row));
            spring_snap(&F.gcol, FX(i % GRID_COLS));
            spring_snap(&F.scroll, FX(F.top));
        } else {
            spring_to(&F.hl, FX(row));
            spring_to(&F.gcol, FX(i % GRID_COLS));
            spring_to(&F.scroll, FX(F.top));
        }
        F.prev_wait = 10;
        F.prev_ok = 0;
        F.prev_for = -1;
        return;
    }
    if (F.sel < F.top) F.top = F.sel;
    if (F.sel >= F.top + LIST_ROWS) F.top = F.sel - LIST_ROWS + 1;
    if (F.top > 0 && F.top > F.n - LIST_ROWS) F.top = F.n > LIST_ROWS ? F.n - LIST_ROWS : 0;
    if (snap) {
        spring_snap(&F.hl, FX(F.sel));
        spring_snap(&F.scroll, FX(F.top));
    } else {
        spring_to(&F.hl, FX(F.sel));
        spring_to(&F.scroll, FX(F.top));
    }
    F.prev_wait = 10; /* a picture's preview is decoded once the focus rests */
    F.prev_ok = 0;
    F.prev_for = -1;
}

static void touch_recent(const char *dir)
{
    if (!dir[0] || is_root(dir))
        return;
    int at = FM_PLACES - 1;
    for (int i = 0; i < FM_PLACES; i++)
        if (!strcasecmp(F.recent[i], dir)) { at = i; break; }
    for (int i = at; i > 0; i--)
        memcpy(F.recent[i], F.recent[i - 1], FOP_PATH);
    snprintf(F.recent[0], FOP_PATH, "%s", dir);
}

static void load_dir(const char *path, const char *select)
{
    snprintf(F.cwd, sizeof(F.cwd), "%s", path);
    snprintf(F.select_name, sizeof(F.select_name), "%s", select ? select : "");
    F.loading = 1;
    F.confirm_delete = 0;
    static PathArg a;
    a.gen = ++F.gen;
    snprintf(a.path, sizeof(a.path), "%s", path);
    bg_post(job_list, &a, sizeof(a));
    if (!path[0])
        bg_post(job_devinfo, NULL, 0);
}

static void reload(void)
{
    const FEnt *e = cur();
    load_dir(F.cwd, e ? e->name : NULL);
}

void fm_open(const char *path)
{
    F.open = 1;
    spring_to(&F.t, FX(1));
    if (path) {
        char sel[NAME_LEN] = "";
        load_dir(path, sel);
    } else {
        reload();
    }
}

int fm_is_open(void) { return F.open || F.t.x > 0; }
int fm_covers(void) { return F.open && F.t.x >= FX(1); }
int fm_busy(void) { return fop_busy(); }

static void close_fm(void)
{
    F.open = 0;
    spring_to(&F.t, 0);
    static char r[FM_PLACES][FOP_PATH];
    memcpy(r, F.recent, sizeof(r));
    bg_post(job_save_recent, r, sizeof(r));
}

/* ---------------------------------------------------------------- places */

int fm_place_count(void)
{
    int n = 1 + (F.has_ef0 ? 1 : 0);
    for (int i = 0; i < FM_PLACES && n < FM_PLACES; i++)
        if (F.recent[i][0]) n++;
    return n;
}

static int place_recent(int i) /* index into recent, or -1 for a device */
{
    int dev = 1 + (F.has_ef0 ? 1 : 0);
    if (i < dev) return -1;
    int k = i - dev;
    for (int r = 0; r < FM_PLACES; r++)
        if (F.recent[r][0] && k-- == 0) return r;
    return -1;
}

const char *fm_place_path(int i)
{
    int r = place_recent(i);
    if (r >= 0) return F.recent[r];
    return i == 1 ? "ef0:/" : "ms0:/";
}

const char *fm_place_name(int i)
{
    int r = place_recent(i);
    if (r < 0) return i == 1 ? "Internal storage" : "Memory Stick";
    return fop_basename(F.recent[r]);
}

int fm_place_icon(int i) { return place_recent(i) < 0 ? ICON_STORAGE : ICON_FOLDER; }

/* ------------------------------------------------------------- selection */

static int marked_count(uint64_t *bytes)
{
    int n = 0;
    uint64_t b = 0;
    for (int i = 0; i < F.n; i++)
        if (s_ents[i].marked) {
            n++;
            b += s_ents[i].size;
        }
    if (bytes) *bytes = b;
    return n;
}

/* The marked items, or the focused one: full paths into s_work. */
static int gather(void)
{
    int n = 0;
    for (int i = 0; i < F.n && n < FOP_MAX_ITEMS; i++)
        if (s_ents[i].marked && join(s_work[n], F.cwd, s_ents[i].name))
            n++;
    if (!n && cur() && join(s_work[0], F.cwd, cur()->name))
        n = 1;
    return n;
}

static int touches_self(int n)
{
    for (int i = 0; i < n; i++)
        if (fop_is_inside(F.self_dir, s_work[i]))
            return 1;
    return 0;
}

static int is_system(int n)
{
    for (int i = 0; i < n; i++) {
        const char *p = s_work[i];
        if (!strcasecmp(p + 5, "PSP") || !strcasecmp(p + 5, "PSP/GAME") || !strcasecmp(p + 5, "PSP/SAVEDATA"))
            return 1;
    }
    return 0;
}

/* --------------------------------------------------------------- actions */

static const ViewerSource s_pic_src;
static int s_pics[FM_MAX], s_npics;

static int pic_count(void) { return s_npics; }
static int pic_get(int i, Entry *e)
{
    if (i < 0 || i >= s_npics || F.loading)
        return -1;
    const FEnt *f = &s_ents[s_pics[i]];
    memset(e, 0, sizeof(*e));
    if (!join(e->path, F.cwd, f->name) || strlen(e->path) >= sizeof(e->path))
        return -1;
    snprintf(e->title, sizeof(e->title), "%s", f->name);
    e->kind = KIND_PICTURE;
    e->size = f->size;
    e->mtime = f->mtime;
    return 0;
}
static const ViewerSource s_pic_src = {pic_count, pic_get};

static int is_zip(const FEnt *e) { return e->type == FT_ARCHIVE && !strcasecmp(ext_of(e->name), "zip"); }

static void open_entry(const FEnt *e, int mode)
{
    char path[FOP_PATH];
    if (e->type == FT_DEVICE) {
        load_dir(e->name, NULL);
        return;
    }
    if (!join(path, F.cwd, e->name)) {
        ui_toast("The path is too long");
        return;
    }
    if (e->type == FT_DIR) {
        touch_recent(path);
        load_dir(path, NULL);
        return;
    }
    if (mode >= 0) {
        reader_open(path, mode);
        return;
    }
    if (is_zip(e)) { /* ✕: what can be done with it */
        open_menu();
        return;
    }
    int long_path = strlen(path) >= sizeof(((Entry *)0)->path);
    switch (e->type) {
    case FT_PICTURE: {
        if (long_path) break;
        s_npics = 0;
        int at = 0;
        for (int i = 0; i < F.n; i++)
            if (s_ents[i].type == FT_PICTURE) {
                if (i == F.sel) at = s_npics;
                s_pics[s_npics++] = i;
            }
        viewer_open_source(&s_pic_src, at);
        return;
    }
    case FT_MUSIC: {
        int n = 0, at = 0;
        for (int i = 0; i < F.n; i++)
            if (s_ents[i].type == FT_MUSIC) n++;
        char (*paths)[FOP_PATH] = malloc((size_t)n * FOP_PATH);
        const char **ptrs = malloc(sizeof(char *) * n);
        if (!paths || !ptrs) {
            free(paths);
            free(ptrs);
            ui_toast("Not enough memory");
            return;
        }
        int k = 0;
        for (int i = 0; i < F.n; i++)
            if (s_ents[i].type == FT_MUSIC && join(paths[k], F.cwd, s_ents[i].name) &&
                strlen(paths[k]) < sizeof(((Entry *)0)->path)) {
                if (i == F.sel) at = k;
                ptrs[k] = paths[k];
                k++;
            }
        music_play_files(ptrs, k, at); /* the player copies the list */
        free(paths);
        free(ptrs);
        ui_toast(k > 1 ? "Playing this folder" : "Playing");
        return;
    }
    case FT_VIDEO: {
        if (long_path) break;
        Entry v;
        memset(&v, 0, sizeof(v));
        snprintf(v.path, sizeof(v.path), "%s", path);
        snprintf(v.title, sizeof(v.title), "%.*s", (int)sizeof(v.title) - 1, e->name);
        v.kind = KIND_VIDEO;
        video_open(&v);
        return;
    }
    case FT_DISC:
    case FT_PBP: {
        if (long_path) break;
        static ProbeArg a;
        snprintf(a.path, sizeof(a.path), "%s", path);
        s_probe_ready = 0;
        bg_post(job_probe, &a, sizeof(a));
        return;
    }
    case FT_TEXT:
    case FT_PDF:
        reader_open(path, READER_AUTO);
        return;
    default:
        reader_open(path, READER_AUTO); /* looks at the bytes: text or hex */
        return;
    }
    ui_toast("The path is too long to open");
}

static void go_up(void)
{
    if (!F.cwd[0]) {
        close_fm();
        return;
    }
    char parent[FOP_PATH], child[NAME_LEN];
    parent_of(F.cwd, parent, child);
    if (is_root(F.cwd)) /* back to the device list, on the device we came from */
        snprintf(child, sizeof(child), "%s", F.cwd);
    load_dir(parent, child);
}

static int s_own_job; /* the running file job is the file manager's (Save data starts its own) */

/* The ZIP the △ menu was opened on: where a homebrew inside it belongs. */
static struct {
    int ok;              /* there is an Install row */
    int exists;          /* it's there already: Install replaces it, after a second ✕ */
    int confirm;
    char dest[FOP_PATH]; /* unpack here */
    char name[64];       /* what to call it on the row */
} s_zip;

static int path_exists(const char *p)
{
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    return sceIoGetstat(p, &st) >= 0;
}

static int zip_read_file(void *ctx, uint64_t off, void *dst, uint32_t len)
{
    SceUID fd = *(SceUID *)ctx;
    if (sceIoLseek(fd, (SceOff)off, PSP_SEEK_SET) < 0)
        return -1;
    return sceIoRead(fd, dst, len);
}

/* "name.zip" -> "name" */
static void zip_stem(const FEnt *e, char *out, int n)
{
    const char *dot = strrchr(e->name, '.');
    snprintf(out, n, "%.*s", dot ? (int)(dot - e->name) : (int)strlen(e->name), e->name);
}

/* Looks inside the focused ZIP for a homebrew, before the menu shows. */
static void zip_prepare(const FEnt *e)
{
    memset(&s_zip, 0, sizeof(s_zip));
    char path[FOP_PATH], stem[64];
    if (!e || !is_zip(e) || !join(path, F.cwd, e->name))
        return;
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return;
    ZipDir z;
    ZipPlan p = {0};
    if (zip_open(&z, zip_read_file, &fd, (uint64_t)sceIoLseek(fd, 0, PSP_SEEK_END)) == ZIP_OK) {
        zip_install_plan(&z, &p);
        zip_close(&z);
    }
    sceIoClose(fd);
    if (p.kind == ZIP_INSTALL_NONE)
        return;
    char games[32], target[FOP_PATH];
    snprintf(games, sizeof(games), "%.5sPSP/GAME", F.cwd);
    if (p.kind == ZIP_INSTALL_BARE) {
        zip_stem(e, stem, sizeof(stem));
        snprintf(p.name, sizeof(p.name), "%s", stem);
    }
    if (snprintf(target, sizeof(target), "%s/%s", games, p.name) >= (int)sizeof(target))
        return;
    if (fop_is_inside(target, F.self_dir) || fop_is_inside(F.self_dir, target))
        return; /* XLT itself: not while it is running from there */
    s_zip.exists = path_exists(target);
    snprintf(s_zip.name, sizeof(s_zip.name), "%s", p.name);
    if (p.kind == ZIP_INSTALL_PSP_GAME) snprintf(s_zip.dest, sizeof(s_zip.dest), "%.5s", F.cwd);
    else if (p.kind == ZIP_INSTALL_FOLDER) snprintf(s_zip.dest, sizeof(s_zip.dest), "%s", games);
    else snprintf(s_zip.dest, sizeof(s_zip.dest), "%s", target);
    s_zip.ok = 1;
}

static void start_job(int op, int n, const char *dest)
{
    const char *why = NULL;
    if (sysctl_usb_active()) {
        ui_toast("Turn USB off first");
        return;
    }
    if (fop_start(op, (const char (*)[FOP_PATH])s_work, n, dest, &why) < 0) {
        ui_toast(why ? why : "Can't start that");
        return;
    }
    s_own_job = 1;
    power_boost();
}

static void on_rename(const char *text)
{
    const FEnt *e = cur();
    if (!text || !e || !strcmp(text, e->name))
        return;
    static RenArg r;
    r.kind = 0;
    if (!join(r.a, F.cwd, e->name) || !join(r.b, F.cwd, text)) {
        ui_toast("The name is too long");
        return;
    }
    for (int i = 0; i < F.n; i++)
        if (!strcasecmp(s_ents[i].name, text) && strcasecmp(e->name, text)) {
            ui_toast("Something here already has that name");
            return;
        }
    s_ren_done = 0;
    bg_post(job_rename, &r, sizeof(r));
    load_dir(F.cwd, text);
    F.rescan_pending = 1;
}

static void on_mkdir(const char *text)
{
    if (!text)
        return;
    static RenArg r;
    r.kind = 1;
    if (!join(r.a, F.cwd, text)) {
        ui_toast("The name is too long");
        return;
    }
    for (int i = 0; i < F.n; i++)
        if (!strcasecmp(s_ents[i].name, text)) {
            ui_toast("Something here already has that name");
            return;
        }
    s_ren_done = 0;
    bg_post(job_rename, &r, sizeof(r));
    load_dir(F.cwd, text);
}

enum {
    A_OPEN, A_OPEN_TEXT, A_OPEN_HEX, A_COPY, A_CUT, A_PASTE, A_RENAME, A_NEWDIR, A_DELETE,
    A_MARK_ALL, A_UNMARK, A_SORT, A_VIEW, A_SIZE, A_CANCEL, A_CLEAR_CLIP, A_ZIP_INSTALL, A_ZIP_FOLDER, A_ZIP_HERE,
};

static void fill_menu(MenuItem *it, int *count);
static void job_save_ini(void *arg) { globalcfg_save(); }

static void menu_cb(int id, int dir)
{
    const FEnt *e = cur();
    int n;
    switch (id) {
    case A_OPEN:
        menu_close();
        if (e) open_entry(e, -1);
        return;
    case A_OPEN_TEXT:
    case A_OPEN_HEX:
        menu_close();
        if (e) open_entry(e, id == A_OPEN_TEXT ? READER_TEXT : READER_HEX);
        return;
    case A_COPY:
    case A_CUT:
        n = gather();
        if (!n) return;
        if (id == A_CUT && (touches_self(n) || is_system(n))) {
            ui_toast(touches_self(n) ? "XLT runs from there: it can't be moved" : "The system needs that folder");
            return;
        }
        memcpy(s_clip, s_work, (size_t)n * FOP_PATH);
        F.clip_n = n;
        F.clip_cut = id == A_CUT;
        for (int i = 0; i < F.n; i++)
            s_ents[i].marked = 0;
        menu_close();
        ui_toast(id == A_CUT ? "Cut: go to a folder and paste" : "Copied: go to a folder and paste");
        return;
    case A_PASTE:
        menu_close();
        memcpy(s_work, s_clip, (size_t)F.clip_n * FOP_PATH);
        start_job(F.clip_cut ? FOP_MOVE : FOP_COPY, F.clip_n, F.cwd);
        return;
    case A_CLEAR_CLIP:
        F.clip_n = 0;
        break;
    case A_RENAME:
        menu_close();
        if (e) {
            char p[FOP_PATH];
            join(p, F.cwd, e->name);
            if (fop_is_inside(F.self_dir, p)) {
                ui_toast("XLT runs from there: it can't be renamed");
                return;
            }
            snprintf(s_work[0], FOP_PATH, "%s", p);
            if (is_system(1)) {
                ui_toast("The system needs that folder");
                return;
            }
            osk_open("Rename", e->name, OSK_FILENAME, on_rename);
        }
        return;
    case A_NEWDIR:
        menu_close();
        osk_open("New folder", "New folder", OSK_FILENAME, on_mkdir);
        return;
    case A_DELETE:
        n = gather();
        if (!n) return;
        if (touches_self(n) || is_system(n)) {
            ui_toast(touches_self(n) ? "XLT runs from there: it can't be deleted" : "The system needs that folder");
            return;
        }
        if (!F.confirm_delete) {
            F.confirm_delete = 1;
            break;
        }
        F.confirm_delete = 0;
        menu_close();
        start_job(FOP_DELETE, n, NULL);
        return;
    case A_MARK_ALL:
        for (int i = 0; i < F.n; i++)
            s_ents[i].marked = 1;
        break;
    case A_UNMARK:
        for (int i = 0; i < F.n; i++)
            s_ents[i].marked = 0;
        break;
    case A_SORT:
        F.sort = (F.sort + (dir < 0 ? SORT_COUNT - 1 : 1)) % SORT_COUNT;
        {
            char keep[NAME_LEN];
            snprintf(keep, sizeof(keep), "%s", e ? e->name : "");
            s_sort = F.sort;
            qsort(s_ents, F.n, sizeof(FEnt), cmp_ent);
            int at = 0;
            for (int i = 0; i < F.n; i++)
                if (!strcmp(s_ents[i].name, keep)) at = i;
            F.sel = -1;
            select_row(at, 1);
        }
        break;
    case A_VIEW: /* list <-> grid, for every folder */
        g_cfg.grid ^= GRID_FILES;
        F.top = 0;
        select_row(F.sel, 1);
        bg_post(job_save_ini, NULL, 0);
        break;
    case A_SIZE:
        menu_close();
        if (e && join(s_work[0], F.cwd, e->name)) {
            snprintf(F.size_path, sizeof(F.size_path), "%s", s_work[0]);
            F.size_pending = 1;
            start_job(FOP_SIZE, 1, NULL);
        }
        return;
    case A_CANCEL:
        menu_close();
        fop_cancel();
        return;
    case A_ZIP_INSTALL:
        if (!e || !join(s_work[0], F.cwd, e->name)) return;
        if (s_zip.exists && !s_zip.confirm) {
            s_zip.confirm = 1;
            break;
        }
        menu_close();
        start_job(s_zip.exists ? FOP_EXTRACT_OVER : FOP_EXTRACT, 1, s_zip.dest);
        return;
    case A_ZIP_FOLDER: {
        char stem[NAME_LEN], name[NAME_LEN], dest[FOP_PATH];
        if (!e || !join(s_work[0], F.cwd, e->name)) return;
        zip_stem(e, stem, sizeof(stem));
        fop_unique_name(F.cwd, stem, name, sizeof(name), path_exists);
        menu_close();
        if (!join(dest, F.cwd, name)) {
            ui_toast("The path is too long");
            return;
        }
        start_job(FOP_EXTRACT, 1, dest);
        return;
    }
    case A_ZIP_HERE:
        if (!e || !join(s_work[0], F.cwd, e->name)) return;
        menu_close();
        start_job(FOP_EXTRACT, 1, F.cwd);
        return;
    }
    int cnt;
    MenuItem *items = menu_items(&cnt);
    fill_menu(items, &cnt);
    menu_set_count(cnt);
}

static void fill_menu(MenuItem *it, int *count)
{
    int k = 0;
    const FEnt *e = cur();
    uint64_t mb;
    int marks = marked_count(&mb);
    int in_dir = F.cwd[0] != 0;
    char lbl[sizeof(it[0].label)];
#define ADD(i, l) do { memset(&it[k], 0, sizeof(MenuItem)); it[k].id = (i); \
        snprintf(it[k].label, sizeof(it[k].label), "%s", (l)); k++; } while (0)
    if (fop_busy()) {
        FopStatus st;
        fop_status(&st);
        ADD(A_CANCEL, st.op == FOP_DELETE ? "Stop deleting" : st.op == FOP_MOVE ? "Stop moving"
                      : st.op == FOP_SIZE ? "Stop counting"
                      : st.op == FOP_EXTRACT || st.op == FOP_EXTRACT_OVER ? "Stop extracting" : "Stop copying");
        it[k - 1].danger = 1;
    }
    if (e && !marks && is_zip(e) && !fop_busy()) {
        if (s_zip.ok) {
            if (s_zip.confirm) snprintf(lbl, sizeof(lbl), "Press X again to replace %.20s", s_zip.name);
            else if (s_zip.exists) snprintf(lbl, sizeof(lbl), "Replace %.28s", s_zip.name);
            else snprintf(lbl, sizeof(lbl), "Install to PSP/GAME");
            ADD(A_ZIP_INSTALL, lbl);
            it[k - 1].danger = s_zip.exists;
        }
        ADD(A_ZIP_FOLDER, "Extract to a new folder");
        ADD(A_ZIP_HERE, "Extract here");
    }
    if (e && !marks) {
        if (!is_zip(e)) ADD(A_OPEN, "Open");
        if (e->type != FT_DIR && e->type != FT_DEVICE) {
            if (e->type != FT_TEXT) ADD(A_OPEN_TEXT, "Open as text");
            ADD(A_OPEN_HEX, "Open as hex");
        }
    }
    if (in_dir && (e || marks)) {
        if (marks > 1) snprintf(lbl, sizeof(lbl), "Copy %d items", marks);
        else snprintf(lbl, sizeof(lbl), "Copy");
        ADD(A_COPY, lbl);
        it[k - 1].gap = k > 1;
        if (marks > 1) snprintf(lbl, sizeof(lbl), "Cut %d items", marks);
        else snprintf(lbl, sizeof(lbl), "Cut");
        ADD(A_CUT, lbl);
    }
    if (in_dir && F.clip_n) {
        if (F.clip_n > 1) snprintf(lbl, sizeof(lbl), "Paste %d items here", F.clip_n);
        else snprintf(lbl, sizeof(lbl), "Paste here");
        ADD(A_PASTE, lbl);
    }
    if (in_dir && e && marks <= 1)
        ADD(A_RENAME, "Rename...");
    if (in_dir)
        ADD(A_NEWDIR, "New folder...");
    if (in_dir && (e || marks)) {
        if (F.confirm_delete)
            snprintf(lbl, sizeof(lbl), "Press X again to delete");
        else if (marks > 1)
            snprintf(lbl, sizeof(lbl), "Delete %d items", marks);
        else
            snprintf(lbl, sizeof(lbl), "Delete");
        ADD(A_DELETE, lbl);
        it[k - 1].danger = 1;
    }
    if (in_dir && F.n) {
        ADD(marks == F.n ? A_UNMARK : A_MARK_ALL, marks == F.n ? "Unmark all" : "Mark all");
        it[k - 1].gap = 1;
        if (marks && marks < F.n) ADD(A_UNMARK, "Unmark all");
    }
    if (in_dir) {
        ADD(A_SORT, "Sort by");
        snprintf(it[k - 1].value, sizeof(it[k - 1].value), "%s", sort_name(F.sort));
        if (!F.n) it[k - 1].gap = 1;
    }
    ADD(A_VIEW, "View as");
    snprintf(it[k - 1].value, sizeof(it[k - 1].value), "%s", is_grid() ? "Grid" : "List");
    if (!in_dir) it[k - 1].gap = k > 1;
    if (in_dir && e && e->type == FT_DIR && !marks)
        ADD(A_SIZE, "Folder size");
    if (F.clip_n)
        ADD(A_CLEAR_CLIP, "Empty the clipboard");
#undef ADD
    *count = k;
}

static void open_menu(void)
{
    MenuItem items[MENU_MAX];
    int n;
    F.confirm_delete = 0;
    zip_prepare(cur());
    fill_menu(items, &n);
    const FEnt *e = cur();
    uint64_t mb;
    int marks = marked_count(&mb);
    char sub[96];
    if (marks) {
        char sz[16];
        format_size(sz, sizeof(sz), mb);
        snprintf(sub, sizeof(sub), "%d marked" DOT "%s", marks, sz);
    } else {
        snprintf(sub, sizeof(sub), "%s", e ? type_name(e) : "");
    }
    menu_open(marks ? "Marked items" : e ? (e->type == FT_DEVICE ? (e->name[0] == 'e' ? "Internal storage" : "Memory Stick") : e->name)
                                         : (F.cwd[0] ? fop_basename(F.cwd) : "Storage"),
              sub, items, n, menu_cb);
}

/* --------------------------------------------------------------- previews */

/* The focused file as a library-style Entry: same path, size and date, so the
 * same key, and so the same cached still frame as in the Video tab. */
static const Entry *entry_for(const FEnt *f)
{
    static Entry e;
    static uint32_t built_gen = 0xFFFFFFFFu;
    static int built_sel = -1;
    if (!f || (f->type != FT_VIDEO && f->type != FT_DISC && f->type != FT_PBP))
        return NULL;
    if (built_gen == (uint32_t)F.gen && built_sel == F.sel && !strcmp(fop_basename(e.path), f->name))
        return &e;
    char p[FOP_PATH];
    if (!join(p, F.cwd, f->name) || strlen(p) >= sizeof(e.path))
        return NULL;
    memset(&e, 0, sizeof(e));
    snprintf(e.path, sizeof(e.path), "%s", p);
    snprintf(e.title, sizeof(e.title), "%.*s", (int)sizeof(e.title) - 1, f->name);
    e.kind = f->type == FT_VIDEO ? KIND_VIDEO : f->type == FT_DISC ? KIND_ISO : KIND_HOMEBREW;
    e.size = f->size;
    e.mtime = f->mtime;
    e.key = entry_key(&e);
    built_gen = (uint32_t)F.gen;
    built_sel = F.sel;
    return &e;
}

const Entry *fm_preview_entry(void)
{
    if (!F.open || F.t.x < FX(1) || F.loading || menu_is_open() || osk_is_open() || viewer_is_open() ||
        reader_is_open() || video_is_open())
        return NULL;
    return entry_for(cur());
}

/* ------------------------------------------------------------------ input */

int fm_input(unsigned int p)
{
    if (!F.open)
        return 0;
    if (p & PSP_CTRL_CIRCLE) {
        go_up();
        return 1;
    }
    if (p & PSP_CTRL_TRIANGLE) {
        if (!F.loading)
            open_menu();
        return 1;
    }
    if (p & PSP_CTRL_START && music_state() != MUSIC_STOPPED) {
        music_toggle();
        return 1;
    }
    if (F.loading || !F.n)
        return 1;
    if (is_grid() && p & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) {
        int s = F.sel;
        if (p & PSP_CTRL_UP) { if (s >= GRID_COLS) select_row(s - GRID_COLS, 0); }
        else if (p & PSP_CTRL_DOWN) {
            if (s + GRID_COLS < F.n) select_row(s + GRID_COLS, 0);
            else if (s / GRID_COLS < (F.n - 1) / GRID_COLS) select_row(F.n - 1, 0); /* into a shorter last row */
        }
        else if (p & PSP_CTRL_LEFT) select_row(s - 1, 0);
        else select_row(s + 1, 0);
    }
    else if (p & PSP_CTRL_UP) select_row(F.sel - 1, 0);
    else if (p & PSP_CTRL_DOWN) select_row(F.sel + 1, 0);
    else if (p & PSP_CTRL_LEFT) select_row(F.sel - LIST_ROWS, 0);
    else if (p & PSP_CTRL_RIGHT) select_row(F.sel + LIST_ROWS, 0);
    else if (p & PSP_CTRL_LTRIGGER) select_row(0, 0);
    else if (p & PSP_CTRL_RTRIGGER) select_row(F.n - 1, 0);
    else if (p & PSP_CTRL_CROSS) open_entry(&s_ents[F.sel], -1);
    else if (p & PSP_CTRL_SQUARE && F.cwd[0]) {
        s_ents[F.sel].marked ^= 1;
        select_row(F.sel + 1, 0);
    }
    return 1;
}

/* ------------------------------------------------------------------- step */

static int step(Spring *s)
{
    int old = s->x;
    spring_step(s);
    return s->x != old;
}

static void finish_job(void)
{
    FopStatus st;
    if (!s_own_job || !fop_take_result(&st))
        return;
    s_own_job = 0;
    if (!library_scanning())
        power_idle();
    char msg[128];
    if (st.op == FOP_SIZE) {
        F.size_pending = 0;
        F.size_bytes = st.bytes_total;
        F.size_files = st.items_total;
        if (st.cancelled) F.size_path[0] = 0;
        return;
    }
    const char *verb = st.op == FOP_DELETE ? "Deleted" : st.op == FOP_MOVE ? "Moved"
                     : st.op == FOP_EXTRACT || st.op == FOP_EXTRACT_OVER ? "Extracted" : "Copied";
    if (st.full)
        snprintf(msg, sizeof(msg), "Stopped: the Memory Stick is full");
    else if (st.cancelled)
        snprintf(msg, sizeof(msg), "Stopped" DOT "%s %d of %d", verb, st.items_done, st.items_total);
    else if (st.errors == 1)
        snprintf(msg, sizeof(msg), "%s", st.first_error);
    else if (st.errors)
        snprintf(msg, sizeof(msg), "%s (and %d more)", st.first_error, st.errors - 1);
    else
        snprintf(msg, sizeof(msg), "%s %d item%s", verb, st.items_done, st.items_done == 1 ? "" : "s");
    ui_toast(msg);
    if (st.op == FOP_MOVE && !st.errors && !st.cancelled)
        F.clip_n = 0; /* moved: the clipboard's paths are gone */
    F.rescan_pending = 1;
    F.reload_pending = 1;
    if (st.op != FOP_DELETE && st.dest_name[0])
        snprintf(F.select_name, sizeof(F.select_name), "%s", st.dest_name);
}

int fm_step(void)
{
    int dirty = step(&F.t);
    if (s_ef0_probe >= 0) {
        F.has_ef0 |= s_ef0_probe;
        s_ef0_probe = -1;
        dirty = 1;
    }
    if (s_recent_ready) {
        s_recent_ready = 0;
        memcpy(F.recent, s_recent_load, sizeof(F.recent));
        dirty = 1;
    }
    if (s_list_ready) {
        s_list_ready = 0;
        __sync_synchronize();
        F.has_ef0 = s_list_ef0 || F.has_ef0;
        if (s_list_gen == F.gen) {
            F.loading = 0;
            F.n = s_list_n;
            F.truncated = s_list_trunc;
            F.skipped = s_list_skipped;
            F.err = s_list_err;
            s_sort = F.sort;
            qsort(s_ents, F.n, sizeof(FEnt), cmp_ent);
            int at = 0;
            for (int i = 0; F.select_name[0] && i < F.n; i++)
                if (!strcasecmp(s_ents[i].name, F.select_name)) { at = i; break; }
            F.sel = -1;
            F.top = is_grid() ? 0 : at > LIST_ROWS / 2 ? at - LIST_ROWS / 2 : 0;
            select_row(at, 1);
            F.select_name[0] = 0;
            dirty = 1;
        }
    }
    if (s_dev_ready) {
        s_dev_ready = 0;
        memcpy(F.dev_free, s_dev_free, sizeof(F.dev_free));
        memcpy(F.dev_total, s_dev_total, sizeof(F.dev_total));
        memcpy(F.dev_ok, s_dev_ok, sizeof(F.dev_ok));
        F.has_ef0 |= F.dev_ok[1]; /* not measured is not "missing" */
        dirty = 1;
    }
    if (s_ren_done) {
        s_ren_done = 0;
        if (s_ren_err)
            ui_toast("Couldn't do that: the name may be taken or not allowed");
        dirty = 1;
    }
    if (s_probe_ready) {
        s_probe_ready = 0;
        if (s_probe_ok)
            ui_request_launch(&s_probe);
        else
            ui_toast("This isn't a game or app the PSP can start");
        dirty = 1;
    }
    if (s_prev_ready) {
        s_prev_ready = 0;
        if (s_prev_gen == F.gen * 4096 + F.sel && s_prev_okv) {
            sceKernelDcacheWritebackRange(s_prev_rgba, sizeof(s_prev_rgba));
            F.prev_ok = 1;
            F.prev_w = s_prev_wv;
            F.prev_h = s_prev_hv;
        }
        dirty = 1;
    }
    if (F.prev_wait > 0 && --F.prev_wait == 0 && F.open && cur() && cur()->type == FT_PICTURE && !viewer_is_open()) {
        static PrevArg a;
        a.gen = F.gen * 4096 + F.sel;
        if (join(a.path, F.cwd, cur()->name)) {
            F.prev_for = F.sel;
            bg_post(job_preview, &a, sizeof(a));
        }
    }
    static int tick;
    if (fop_busy() && F.open && (++tick & 7) == 0)
        dirty = 1; /* the progress bar moves: redraw a few times a second */
    finish_job();
    if (F.reload_pending && F.open && !F.loading && !viewer_is_open()) {
        F.reload_pending = 0;
        const FEnt *e = cur();
        char keep[NAME_LEN];
        snprintf(keep, sizeof(keep), "%s", F.select_name[0] ? F.select_name : e ? e->name : "");
        load_dir(F.cwd, keep);
        dirty = 1;
    }
    if (F.rescan_pending && !library_scanning() && !fop_busy()) {
        F.rescan_pending = 0;
        library_rescan();
    }
    dirty |= step(&F.hl);
    dirty |= step(&F.scroll);
    dirty |= step(&F.gcol);
    return dirty;
}

/* ------------------------------------------------------------------- draw */

/* The end of a path that fits in w: "...PSP/GAME/Folder". */
static void fit_tail(char *out, int n, const char *s, int font, int w)
{
    if (gfx_text_width(font, s) <= w) {
        snprintf(out, n, "%s", s);
        return;
    }
    const char *p = s;
    while (*p) {
        p++;
        while (((unsigned char)*p & 0xC0) == 0x80) p++;
        snprintf(out, n, "...%s", p);
        if (gfx_text_width(font, out) <= w)
            return;
    }
    snprintf(out, n, "...");
}

static void draw_wrapped2(int x, int y, int w, const char *s, u32 c)
{
    /* two lines, broken anywhere (file names have few spaces) */
    int lh = gfx_line_h(FONT_BOLD);
    if (gfx_text_width(FONT_BOLD, s) <= w) {
        gfx_text(FONT_BOLD, x, y, s, c);
        return;
    }
    const char *p = s, *cut = s;
    int ww = 0;
    while (*p) {
        const char *q = p;
        int adv = gfx_glyph_adv(FONT_BOLD, &q);
        if (ww + adv > w) break;
        ww += adv;
        p = q;
        cut = p;
    }
    gfx_text_n(FONT_BOLD, x, y, s, (int)(cut - s), c);
    gfx_text_fit(FONT_BOLD, x, y + lh, w, cut, c);
}

static void draw_progress(int x, int y, int w)
{
    FopStatus st;
    fop_status(&st);
    char line[96];
    const char *verb = st.op == FOP_DELETE ? "Deleting" : st.op == FOP_MOVE ? "Moving"
                     : st.op == FOP_SIZE ? "Counting"
                     : st.op == FOP_EXTRACT || st.op == FOP_EXTRACT_OVER ? "Extracting" : "Copying";
    if (st.phase == FOP_COUNTING || st.op == FOP_SIZE)
        snprintf(line, sizeof(line), "%s... %d files", st.op == FOP_SIZE ? "Counting" : "Preparing", st.items_total);
    else
        snprintf(line, sizeof(line), "%s %d of %d", verb, st.items_done + 1 > st.items_total ? st.items_total
                 : st.items_done + 1, st.items_total);
    gfx_rrect(x, y, w, 38, 6, C_RAISED);
    gfx_text_fit(FONT_SMALL, x + 8, y + 3, w - 16, line, C_TEXT);
    int pct = 0;
    if (st.phase == FOP_WORKING) {
        if (st.bytes_total) pct = (int)(st.bytes_done * 100 / st.bytes_total);
        else if (st.items_total) pct = st.items_done * 100 / st.items_total;
    }
    if (pct > 100) pct = 100;
    gfx_rect(x + 8, y + 27, w - 16, 4, C_BG);
    gfx_rect(x + 8, y + 27, (w - 16) * pct / 100, 4, C_FOCUS);
}

static void draw_panel(void)
{
    int x = PANEL_X, w = PANEL_W, y = LIST_Y + 2;
    const FEnt *e = cur();
    if (!e)
        return;
    int lh = gfx_line_h(FONT_SMALL);
    char line[96], sz[16];
    /* preview box */
    if (e->type == FT_PICTURE && F.prev_ok && F.prev_for == F.sel) {
        gfx_rrect(x, y, w, PREV_H, 6, C_SURFACE);
        gfx_texture(s_prev_rgba, GU_PSM_8888, 256, 128, PREV_W, 0, F.prev_w, F.prev_h,
                    x + (w - F.prev_w) / 2, y + (PREV_H - F.prev_h) / 2, F.prev_w, F.prev_h);
    } else {
        const Entry *pe = entry_for(e);
        const void *tex = pe && pe->kind == KIND_VIDEO ? thumbs_get(pe) : NULL;
        if (pe && preview_draw(pe, x, y, w, PREV_H)) { /* playing: the focused video, or a game's ICON1 */
            gfx_corners(x, y, w, PREV_H, 6, C_BG);
        } else if (tex) {
            gfx_rect(x, y, w, PREV_H, RGB(0x000000));
            gfx_thumb(tex, x, y, w, PREV_H);
            gfx_corners(x, y, w, PREV_H, 6, C_BG);
        } else {
            if (pe && pe->kind == KIND_VIDEO && thumbs_missing(pe))
                preview_want_thumb(pe);
            gfx_rrect(x, y, w, PREV_H, 6, C_SURFACE);
            gfx_icon(type_icon(e->type), x + (w - 40) / 2, y + (PREV_H - 40) / 2, 40, 40, type_color(e->type));
        }
    }
    y += PREV_H + 8;
    if (e->type == FT_DEVICE) {
        int k = e->name[0] == 'e';
        gfx_text_fit(FONT_BOLD, x, y, w, k ? "Internal storage" : "Memory Stick", C_TEXT);
        y += gfx_line_h(FONT_BOLD) + 2;
        gfx_text(FONT_SMALL, x, y, e->name, C_MUTED);
        y += lh;
        if (F.dev_ok[k]) {
            char t[16];
            format_size(sz, sizeof(sz), F.dev_free[k]);
            format_size(t, sizeof(t), F.dev_total[k]);
            snprintf(line, sizeof(line), "%s free of %s", sz, t);
            gfx_text_fit(FONT_SMALL, x, y, w, line, C_MUTED);
            y += lh + 4;
            int pct = F.dev_total[k] ? (int)((F.dev_total[k] - F.dev_free[k]) * 100 / F.dev_total[k]) : 0;
            gfx_rect(x, y, w, 4, C_RAISED);
            gfx_rect(x, y, w * pct / 100, 4, pct > 90 ? C_LOW : C_FOCUS);
        }
    } else {
        draw_wrapped2(x, y, w, e->name, C_TEXT);
        y += gfx_line_h(FONT_BOLD) * (gfx_text_width(FONT_BOLD, e->name) > w ? 2 : 1) + 2;
        if (e->type == FT_DIR) {
            char p[FOP_PATH];
            join(p, F.cwd, e->name);
            if (F.size_path[0] && !strcmp(p, F.size_path)) {
                if (F.size_pending) {
                    snprintf(line, sizeof(line), "Folder" DOT "counting...");
                } else {
                    format_size(sz, sizeof(sz), F.size_bytes);
                    snprintf(line, sizeof(line), "%s" DOT "%d files", sz, F.size_files);
                }
            } else {
                snprintf(line, sizeof(line), "Folder");
            }
        } else {
            format_size(sz, sizeof(sz), e->size);
            snprintf(line, sizeof(line), "%s" DOT "%s", type_name(e), sz);
        }
        gfx_text_fit(FONT_SMALL, x, y, w, line, C_MUTED);
        y += lh;
        format_date(line, sizeof(line), e->mtime);
        gfx_text_fit(FONT_SMALL, x, y, w, line, C_MUTED);
        y += lh;
    }
    /* marks and clipboard */
    int by = 204;
    uint64_t mb;
    int marks = marked_count(&mb);
    if (fop_busy()) {
        draw_progress(x, by, w);
        return;
    }
    if (marks) {
        format_size(sz, sizeof(sz), mb);
        snprintf(line, sizeof(line), "%d marked" DOT "%s", marks, sz);
        gfx_icon(ICON_CHECK, x, by + 22, 12, 12, C_FOCUS);
        gfx_text_fit(FONT_SMALL, x + 16, by + 19, w - 16, line, C_FOCUS);
    }
    if (F.clip_n) {
        snprintf(line, sizeof(line), "%s %d item%s", F.clip_cut ? "Cut:" : "Copied:", F.clip_n,
                 F.clip_n == 1 ? "" : "s");
        gfx_text_fit(FONT_SMALL, x, by + (marks ? 1 : 19), w, line, C_MUTED);
    }
}

static const char *ent_label(const FEnt *e)
{
    return e->type == FT_DEVICE ? (e->name[0] == 'e' ? "Internal storage" : "Memory Stick") : e->name;
}

/* Cells with an icon and the name; the focus is a filled cell that slides. */
static void draw_grid(void)
{
    const int pitch = CELL_H + CELL_VGAP, bottom = LIST_Y + GRID_ROWS * pitch - CELL_VGAP;
    const int x0 = LIST_X;
    int first = (F.scroll.x >> 8) - 1;
    if (first < 0) first = 0;
    int last = first + GRID_ROWS + 1;
    for (int pass = 0; pass < 2; pass++) { /* backgrounds, then the focus, then contents */
        if (pass == 1) {
            int hx = x0 + ((F.gcol.x * (CELL_W + CELL_GAP)) >> 8);
            int hy = LIST_Y + (((F.hl.x - F.scroll.x) * pitch) >> 8);
            gfx_rrect(hx, hy, CELL_W, CELL_H, 6, C_ACCENT);
        }
        for (int r = first; r <= last; r++) {
            int y = LIST_Y + (((FX(r) - F.scroll.x) * pitch) >> 8);
            if (y >= bottom + 4 || y + CELL_H <= LIST_Y - 4)
                continue;
            for (int c = 0; c < GRID_COLS; c++) {
                int i = r * GRID_COLS + c;
                if (i >= F.n)
                    break;
                const FEnt *e = &s_ents[i];
                int x = x0 + c * (CELL_W + CELL_GAP), on = i == F.sel;
                if (pass == 0) {
                    if (!on)
                        gfx_rrect(x, y, CELL_W, CELL_H, 6, e->marked ? C_MARK : C_SURFACE);
                    continue;
                }
                gfx_icon(type_icon(e->type), x + (CELL_W - 26) / 2, y + 8, 26, 26, on ? C_WHITE : type_color(e->type));
                if (e->marked)
                    gfx_icon(ICON_CHECK, x + 4, y + 4, 12, 12, on ? C_WHITE : C_FOCUS);
                const char *label = ent_label(e);
                int lw = gfx_text_width(FONT_SMALL, label);
                if (lw <= CELL_W - 8)
                    gfx_text_center(FONT_SMALL, x + CELL_W / 2, y + 38, label, on ? C_WHITE : C_TEXT);
                else
                    gfx_text_fit(FONT_SMALL, x + 4, y + 38, CELL_W - 8, label, on ? C_WHITE : C_TEXT);
            }
        }
    }
    gfx_rect(0, 0, PANEL_X - 6, LIST_Y - 2, C_BG);
    gfx_rect(0, bottom + 2, PANEL_X - 6, SCREEN_H - bottom - 2, C_BG); /* to the bottom: the footer is drawn on top */
    int rows = (F.n + GRID_COLS - 1) / GRID_COLS;
    if (rows > GRID_ROWS) { /* where in the folder */
        int h = bottom - LIST_Y, bh = h * GRID_ROWS / rows;
        if (bh < 10) bh = 10;
        int y = LIST_Y + (h - bh) * F.top / (rows - GRID_ROWS);
        gfx_rrect(LIST_X + LIST_W + 3, y, 3, bh, 1, C_RAISED);
    }
    if (F.truncated || F.skipped) {
        char note[64];
        snprintf(note, sizeof(note), F.truncated ? "Only the first %d items are shown" : "%d names are too long to show",
                 F.truncated ? FM_MAX : F.skipped);
        gfx_text_fit(FONT_SMALL, LIST_X + 4, bottom + 2, LIST_W, note, C_FOCUS);
    }
}

static void draw_list(void)
{
    if (is_grid()) {
        draw_grid();
        return;
    }
    const int bottom = LIST_Y + LIST_ROWS * ROW_H;
    int hl_y = LIST_Y + (((F.hl.x - F.scroll.x) * ROW_H) >> 8);
    gfx_rrect(LIST_X, hl_y, LIST_W, ROW_H, 5, C_ACCENT);
    int first = (F.scroll.x >> 8) - 1;
    for (int i = first < 0 ? 0 : first; i < F.n && i <= first + LIST_ROWS + 2; i++) {
        int y = LIST_Y + (((FX(i) - F.scroll.x) * ROW_H) >> 8);
        const FEnt *e = &s_ents[i];
        int on = i == F.sel;
        if (e->marked && !on)
            gfx_rrect(LIST_X, y, LIST_W, ROW_H, 5, C_MARK);
        int x = LIST_X + 6;
        if (e->marked) {
            gfx_icon(ICON_CHECK, x, y + 5, 12, 12, on ? C_WHITE : C_FOCUS);
            x += 15;
        }
        gfx_icon(type_icon(e->type), x, y + 4, 14, 14, on ? C_WHITE : type_color(e->type));
        x += 20;
        char right[24] = "";
        if (e->type == FT_DEVICE)
            snprintf(right, sizeof(right), "%s", e->name);
        else if (e->type != FT_DIR)
            format_size(right, sizeof(right), e->size);
        int rw = right[0] ? gfx_text_width(FONT_SMALL, right) + 10 : 0;
        const char *label = e->type == FT_DEVICE ? (e->name[0] == 'e' ? "Internal storage" : "Memory Stick") : e->name;
        int f = on ? FONT_BOLD : FONT_BODY;
        gfx_text_fit(f, x, y + (ROW_H - gfx_line_h(f)) / 2, LIST_X + LIST_W - 6 - x - rw, label,
                     on ? C_WHITE : e->type == FT_DIR || e->type == FT_DEVICE ? C_TEXT : C_MUTED);
        if (right[0])
            gfx_text_right(FONT_SMALL, LIST_X + LIST_W - 8, y + (ROW_H - gfx_line_h(FONT_SMALL)) / 2 + 1, right,
                           on ? C_WHITE : C_MUTED);
    }
    gfx_rect(0, 0, PANEL_X - 6, LIST_Y - 2, C_BG);
    gfx_rect(0, bottom + 2, PANEL_X - 6, SCREEN_H - bottom - 2, C_BG); /* to the bottom: the footer is drawn on top */
    if (F.n > LIST_ROWS) { /* where in the folder */
        int h = LIST_ROWS * ROW_H, bh = h * LIST_ROWS / F.n;
        if (bh < 10) bh = 10;
        int y = LIST_Y + (h - bh) * F.top / (F.n - LIST_ROWS);
        gfx_rrect(LIST_X + LIST_W + 3, y, 3, bh, 1, C_RAISED);
    }
    if (F.truncated || F.skipped) {
        char note[64];
        snprintf(note, sizeof(note), F.truncated ? "Only the first %d items are shown" : "%d names are too long to show",
                 F.truncated ? FM_MAX : F.skipped);
        gfx_text_fit(FONT_SMALL, LIST_X + 4, bottom + 2, LIST_W, note, C_FOCUS);
    }
}

void fm_draw(const SysStatus *st)
{
    int t = F.t.x;
    if (t <= 0)
        return;
    if (t > 256) t = 256;
    gfx_set_opacity(t);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, C_BG);

    if (F.loading) {
        gfx_text_center(FONT_BODY, SCREEN_W / 2, 126, "Loading...", C_MUTED);
    } else if (F.err) {
        gfx_icon(ICON_FOLDER, SCREEN_W / 2 - 20, 84, 40, 40, C_MUTED);
        gfx_text_center(FONT_BODY, SCREEN_W / 2, 132, "Can't open this folder", C_MUTED);
    } else if (!F.n) {
        gfx_icon(ICON_FOLDER, SCREEN_W / 2 - 20, 84, 40, 40, C_RAISED);
        gfx_text_center(FONT_BODY, SCREEN_W / 2, 132, "This folder is empty", C_MUTED);
        if (fop_busy())
            draw_progress(PANEL_X, 204, PANEL_W);
    } else {
        draw_list();
        draw_panel();
    }

    char title[FOP_PATH + 8];
    if (!F.cwd[0])
        snprintf(title, sizeof(title), "Storage");
    else
        fit_tail(title, sizeof(title), F.cwd, FONT_SMALL, 200);
    ui_draw_status_bar(title, st);

    gfx_rect(0, FOOTER_Y, SCREEN_W, 1, C_LINE);
    static const Hint hints[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Open"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Up"},
        {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Menu"},
        {ICON_BTN_SQUARE, NULL, C_BTN_SQUARE, "Mark"},
        {-1, "L/R", 0, "First / last"},
    };
    static const Hint root_hints[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Open"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
    };
    if (F.cwd[0])
        ui_draw_hints(hints, COUNT(hints), SCREEN_W / 2, FOOTER_Y + 5);
    else
        ui_draw_hints(root_hints, COUNT(root_hints), SCREEN_W / 2, FOOTER_Y + 5);
    gfx_set_opacity(256);
}
