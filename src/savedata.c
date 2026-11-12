/* XLT - Save data. See savedata.h.
 *
 * Two tabs, switched with L/R: Saves (ms0:/PSP/SAVEDATA and ef0:'s) and
 * Backups (<device>/SAVEDATA_BACKUP/<YYYY-MM-DD>/<save folder>). Each folder
 * is read on a thread of its own (a PARAM.SFO for the titles, the files for
 * the size and date), so the screen slides in at once.
 *
 * Back up copies the save's folder into today's backup folder; Restore puts a
 * backup back under the folder name its PARAM.SFO gives (SAVEDATA_DIRECTORY),
 * replacing the save there after a second ✕. The copying and deleting is the
 * file manager's job thread (fileops.c); this screen only watches it.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "savedata.h"
#include "ui_internal.h"
#include "anim.h"
#include "sfx.h"
#include "menu.h"
#include "fileops.h"
#include "sfokey.h"
#include "thumbs.h"
#include "musicsort.h"
#include "sysctl.h"

#define SD_MAX 200
#define ROW_Y 54
#define ROW_H 38
#define ROWS 5
#define ICON_W 64
#define ICON_H 36

enum { TAB_SAVES, TAB_BACKUPS, TABS };

typedef struct {
    char dir[200];          /* the folder */
    char name[64];          /* its name, e.g. ULUS10041DATA00 */
    char title[128];        /* the game (TITLE) */
    char sub[128];          /* the save's own title (SAVEDATA_TITLE) */
    char restore_name[64];  /* backups: the folder name to restore under */
    char day[16];           /* backups: the day folder, "2026-11-08" */
    uint64_t size;
    uint32_t mtime;         /* newest file, packed like Entry.mtime */
    Entry icon;             /* ICON0.PNG, for the thumbnail cache */
    int has_icon;
} SdItem;

/* the scan thread's result, adopted by the UI thread */
static struct {
    volatile int ready;
    SdItem *items[TABS];
    int n[TABS];
} R;
static SceUID s_thread = -1;

static struct {
    int open;
    Spring t;
    int tab;
    SdItem *items[TABS];
    int n[TABS];
    int sel[TABS], top[TABS];
    Spring hl, scroll;
    int loading;
    int menu_for;           /* the item the △ menu is for */
    int confirm;            /* ✕ once on a Delete / Replace row */
} D;

/* the running copy / delete, and what comes after it */
enum { J_NONE, J_BACKUP, J_DELETE, J_RESTORE_CLEAR, J_RESTORE_COPY };
static struct {
    int kind;
    char src[FOP_PATH];     /* the folder being copied (or deleted) */
    char dest[200];         /* where it goes */
    char final_name[64];    /* restore: the name it must end up with */
} J;

/* ------------------------------------------------------------------ scan */

static uint32_t pack_time(const ScePspDateTime *t)
{
    return (uint32_t)((t->year - 1980) & 0x7F) << 25 | (uint32_t)t->month << 21 | (uint32_t)t->day << 16 |
           (uint32_t)t->hour << 11 | (uint32_t)t->minute << 5 | (uint32_t)(t->second >> 1);
}

/* One save folder: its PARAM.SFO, its size and newest date, its icon. 0 if it is one. */
static int read_save(const char *dir, const char *name, SdItem *it)
{
    memset(it, 0, sizeof(*it));
    snprintf(it->dir, sizeof(it->dir), "%.130s/%.63s", dir, name);
    snprintf(it->name, sizeof(it->name), "%.63s", name);
    snprintf(it->restore_name, sizeof(it->restore_name), "%.63s", name);
    SceUID d = sceIoDopen(it->dir);
    if (d < 0)
        return -1;
    SceIoDirent de;
    memset(&de, 0, sizeof(de));
    int has_sfo = 0;
    while (sceIoDread(d, &de) > 0) {
        if (!FIO_S_ISDIR(de.d_stat.st_mode)) {
            it->size += de.d_stat.st_size;
            uint32_t m = pack_time(&de.d_stat.sce_st_mtime);
            if (m > it->mtime) it->mtime = m;
            if (!strcasecmp(de.d_name, "PARAM.SFO")) has_sfo = 1;
            if (!strcasecmp(de.d_name, "ICON0.PNG") && de.d_stat.st_size > 0) {
                Entry *e = &it->icon;
                snprintf(e->path, sizeof(e->path), "%.150s/%.40s", it->dir, de.d_name);
                e->size = de.d_stat.st_size;
                e->mtime = m;
                e->kind = KIND_PICTURE;
                e->icon_size = 1;
                e->key = entry_key(e);
                it->has_icon = 1;
            }
        }
        memset(&de, 0, sizeof(de));
    }
    sceIoDclose(d);
    if (!has_sfo)
        return -1; /* not a save (or a broken one) */
    char p[260];
    snprintf(p, sizeof(p), "%s/PARAM.SFO", it->dir);
    SceUID fd = sceIoOpen(p, PSP_O_RDONLY, 0);
    if (fd >= 0) {
        static uint8_t sfo[8192];
        int n = sceIoRead(fd, sfo, sizeof(sfo));
        sceIoClose(fd);
        if (n > 0) {
            sfo_get(sfo, n, "TITLE", it->title, sizeof(it->title));
            sfo_get(sfo, n, "SAVEDATA_TITLE", it->sub, sizeof(it->sub));
            char rn[64];
            if (sfo_get(sfo, n, "SAVEDATA_DIRECTORY", rn, sizeof(rn)) == 0 && rn[0] && !strchr(rn, '/'))
                snprintf(it->restore_name, sizeof(it->restore_name), "%s", rn);
        }
    }
    if (!it->title[0])
        snprintf(it->title, sizeof(it->title), "%.63s", name);
    return 0;
}

/* The save folders in dir, appended; day labels them (backups). */
static void scan_dir(const char *dir, const char *day, SdItem *out, int *n)
{
    SceUID d = sceIoDopen(dir);
    if (d < 0)
        return;
    SceIoDirent de;
    memset(&de, 0, sizeof(de));
    static char names[SD_MAX][64]; /* read the names first: no folder open while reading each save */
    int k = 0;
    while (k < SD_MAX && sceIoDread(d, &de) > 0) {
        if (FIO_S_ISDIR(de.d_stat.st_mode) && de.d_name[0] != '.' && strlen(de.d_name) < 64)
            snprintf(names[k++], 64, "%s", de.d_name);
        memset(&de, 0, sizeof(de));
    }
    sceIoDclose(d);
    for (int i = 0; i < k && *n < SD_MAX; i++)
        if (read_save(dir, names[i], &out[*n]) == 0) {
            if (day) snprintf(out[*n].day, sizeof(out[*n].day), "%s", day);
            (*n)++;
        }
}

static int by_title(const void *a, const void *b)
{
    const SdItem *x = a, *y = b;
    int r = strcmp(y->day, x->day); /* backups: the newest day first */
    if (!r) r = natural_cmp(x->title, y->title);
    if (!r) r = natural_cmp(x->name, y->name);
    return r;
}

static int scan_thread(SceSize args, void *argp)
{
    static const char *const roots[] = {"ms0:", "ef0:"};
    char dir[64], day_dir[128];
    for (int t = 0; t < TABS; t++) {
        R.items[t] = malloc(sizeof(SdItem) * SD_MAX);
        R.n[t] = 0;
    }
    for (int r = 0; r < 2; r++) {
        if (R.items[TAB_SAVES]) {
            snprintf(dir, sizeof(dir), "%s/PSP/SAVEDATA", roots[r]);
            scan_dir(dir, NULL, R.items[TAB_SAVES], &R.n[TAB_SAVES]);
        }
        if (R.items[TAB_BACKUPS]) { /* SAVEDATA_BACKUP/<day>/<save> */
            snprintf(dir, sizeof(dir), "%s/SAVEDATA_BACKUP", roots[r]);
            SceUID d = sceIoDopen(dir);
            if (d >= 0) {
                SceIoDirent de;
                static char days[64][16];
                int nd = 0;
                memset(&de, 0, sizeof(de));
                while (nd < 64 && sceIoDread(d, &de) > 0) {
                    if (FIO_S_ISDIR(de.d_stat.st_mode) && de.d_name[0] != '.' && strlen(de.d_name) < 16)
                        snprintf(days[nd++], 16, "%s", de.d_name);
                    memset(&de, 0, sizeof(de));
                }
                sceIoDclose(d);
                for (int i = 0; i < nd; i++) {
                    snprintf(day_dir, sizeof(day_dir), "%.60s/%.15s", dir, days[i]);
                    scan_dir(day_dir, days[i], R.items[TAB_BACKUPS], &R.n[TAB_BACKUPS]);
                }
            }
        }
    }
    for (int t = 0; t < TABS; t++)
        if (R.items[t]) qsort(R.items[t], R.n[t], sizeof(SdItem), by_title);
    R.ready = 1;
    return 0;
}

static void rescan(void)
{
    if (s_thread >= 0)
        return; /* one at a time; the screen asks again when it's adopted */
    D.loading = 1;
    R.ready = 0;
    s_thread = sceKernelCreateThread("xl_saves", scan_thread, 0x30, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (s_thread < 0 || sceKernelStartThread(s_thread, 0, NULL) < 0) {
        if (s_thread >= 0) sceKernelDeleteThread(s_thread);
        s_thread = -1;
        D.loading = 0;
    }
}

/* ------------------------------------------------------------------ jobs */

static const SdItem *cur(void)
{
    int t = D.tab, s = D.sel[t];
    return D.items[t] && s >= 0 && s < D.n[t] ? &D.items[t][s] : NULL;
}

static int exists(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

/* The device part of a path: "ms0:". */
static void dev_of(const char *path, char *out, int n)
{
    const char *c = strchr(path, ':');
    snprintf(out, n, "%.*s", c ? (int)(c - path + 1) : 4, c ? path : "ms0:");
}

static int start(int op, const char *src, const char *dest, int kind)
{
    static char paths[1][FOP_PATH];
    const char *why = NULL;
    if (sysctl_usb_active()) {
        ui_toast("Turn USB off first");
        return -1;
    }
    snprintf(paths[0], FOP_PATH, "%s", src);
    if (fop_start(op, (const char (*)[FOP_PATH])paths, 1, dest, &why) < 0) {
        ui_toast(why ? why : fop_busy() ? "Wait for the file job to finish first" : "Can't start that");
        return -1;
    }
    J.kind = kind;
    snprintf(J.src, sizeof(J.src), "%s", src);
    snprintf(J.dest, sizeof(J.dest), "%s", dest ? dest : "");
    power_boost();
    return 0;
}

static void back_up(const SdItem *it)
{
    char dev[8], dir[128];
    ScePspDateTime t;
    sceRtcGetCurrentClockLocalTime(&t);
    dev_of(it->dir, dev, sizeof(dev));
    snprintf(dir, sizeof(dir), "%s/SAVEDATA_BACKUP", dev);
    sceIoMkdir(dir, 0777);
    snprintf(dir, sizeof(dir), "%s/SAVEDATA_BACKUP/%04d-%02d-%02d", dev, t.year, t.month, t.day);
    sceIoMkdir(dir, 0777);
    start(FOP_COPY, it->dir, dir, J_BACKUP); /* a second backup the same day gets " (2)" */
}

static void restore(const SdItem *it)
{
    char dev[8], target[200], there[260];
    dev_of(it->dir, dev, sizeof(dev));
    snprintf(target, sizeof(target), "%s/PSP/SAVEDATA", dev);
    sceIoMkdir(target, 0777);
    snprintf(there, sizeof(there), "%.190s/%.63s", target, it->restore_name);
    snprintf(J.final_name, sizeof(J.final_name), "%s", it->restore_name);
    if (exists(there)) { /* the save there goes first (the menu asked) */
        if (start(FOP_DELETE, there, NULL, J_RESTORE_CLEAR) == 0) {
            snprintf(J.src, sizeof(J.src), "%s", it->dir); /* what's copied next */
            snprintf(J.dest, sizeof(J.dest), "%s", target);
        }
        return;
    }
    start(FOP_COPY, it->dir, target, J_RESTORE_COPY);
}

/* A job ended: say how it went, start what comes after it. Returns 1 if anything changed. */
static int finish_job(void)
{
    FopStatus st;
    if (J.kind == J_NONE || !fop_take_result(&st))
        return 0;
    int kind = J.kind, ok = !st.errors && !st.cancelled && !st.full;
    J.kind = J_NONE;
    power_idle();
    char msg[128];
    if (!ok) {
        snprintf(msg, sizeof(msg), "%s", st.full ? "Stopped: the Memory Stick is full"
                                       : st.cancelled ? "Stopped" : st.first_error[0] ? st.first_error : "That didn't work");
        ui_toast(msg);
        rescan();
        return 1;
    }
    switch (kind) {
    case J_BACKUP: ui_toast("Backed up"); break;
    case J_DELETE: ui_toast("Deleted"); break;
    case J_RESTORE_CLEAR: { /* the old save is gone: now the copy */
        char src[FOP_PATH], dest[200], name[64];
        snprintf(src, sizeof(src), "%s", J.src);
        snprintf(dest, sizeof(dest), "%s", J.dest);
        snprintf(name, sizeof(name), "%s", J.final_name);
        if (start(FOP_COPY, src, dest, J_RESTORE_COPY) == 0)
            snprintf(J.final_name, sizeof(J.final_name), "%s", name);
        return 1;
    }
    case J_RESTORE_COPY: { /* copied under the backup's folder name: give it the save's own */
        if (st.dest_name[0] && strcmp(st.dest_name, J.final_name)) {
            char a[260], b[260];
            snprintf(a, sizeof(a), "%.160s/%.95s", J.dest, st.dest_name);
            snprintf(b, sizeof(b), "%.160s/%.63s", J.dest, J.final_name);
            sceIoRename(a, b);
        }
        ui_toast("Restored: the game will find it there");
        break;
    }
    }
    rescan();
    return 1;
}

/* ------------------------------------------------------------------ menu */

enum { SM_BACKUP = 1, SM_RESTORE, SM_DELETE };

static void fill_menu(MenuItem *it, int *n)
{
    const SdItem *s = D.items[D.tab] && D.menu_for < D.n[D.tab] ? &D.items[D.tab][D.menu_for] : NULL;
    int k = 0;
    memset(it, 0, sizeof(MenuItem) * 3);
    if (D.tab == TAB_SAVES) {
        it[k].id = SM_BACKUP; snprintf(it[k++].label, sizeof(it[0].label), "Back up");
    } else if (s) {
        char dev[8], there[260];
        dev_of(s->dir, dev, sizeof(dev));
        snprintf(there, sizeof(there), "%s/PSP/SAVEDATA/%s", dev, s->restore_name);
        int replaces = exists(there);
        it[k].id = SM_RESTORE;
        it[k].danger = replaces;
        snprintf(it[k++].label, sizeof(it[0].label), "%s",
                 !replaces ? "Restore" : D.confirm == SM_RESTORE ? "Press X again to replace" : "Restore (replaces the save)");
    }
    it[k].id = SM_DELETE; it[k].danger = 1; it[k].gap = 1;
    snprintf(it[k++].label, sizeof(it[0].label), "%s",
             D.confirm == SM_DELETE ? "Press X again to delete" : D.tab == TAB_SAVES ? "Delete save" : "Delete backup");
    *n = k;
}

static void menu_cb(int id, int dir)
{
    if (dir || !D.items[D.tab] || D.menu_for >= D.n[D.tab])
        return;
    const SdItem *s = &D.items[D.tab][D.menu_for];
    int n;
    if (id == SM_BACKUP) {
        menu_close();
        back_up(s);
        return;
    }
    int needs_confirm = id == SM_DELETE || (id == SM_RESTORE && menu_items(NULL)[0].danger);
    if (needs_confirm && D.confirm != id) {
        D.confirm = id;
        fill_menu(menu_items(NULL), &n);
        return;
    }
    D.confirm = 0;
    menu_close();
    if (id == SM_RESTORE) restore(s);
    else start(FOP_DELETE, s->dir, NULL, J_DELETE);
}

static void open_menu(void)
{
    const SdItem *s = cur();
    if (!s)
        return;
    D.menu_for = D.sel[D.tab];
    D.confirm = 0;
    MenuItem items[3];
    int n;
    fill_menu(items, &n);
    char sub[96], size[16];
    format_size(size, sizeof(size), s->size);
    snprintf(sub, sizeof(sub), "%s" DOT "%s", s->name, size);
    menu_open(s->title, sub, items, n, menu_cb);
}

/* ------------------------------------------------------------------ input */

void sd_open(void)
{
    D.open = 1;
    spring_to(&D.t, FX(1));
    rescan();
}

int sd_is_open(void) { return D.open || D.t.x > 0; }
int sd_covers(void) { return D.open && D.t.x >= FX(1); }

static void select_row(int i, int snap)
{
    int t = D.tab, n = D.n[t];
    if (i >= n) i = n - 1;
    if (i < 0) i = 0;
    if (!snap && i != D.sel[t]) sfx_play(SFX_MOVE);
    D.sel[t] = i;
    if (i < D.top[t]) D.top[t] = i;
    if (i >= D.top[t] + ROWS) D.top[t] = i - ROWS + 1;
    if (snap) {
        spring_snap(&D.hl, FX(i));
        spring_snap(&D.scroll, FX(D.top[t]));
    } else {
        spring_to(&D.hl, FX(i));
        spring_to(&D.scroll, FX(D.top[t]));
    }
}

int sd_input(unsigned int p)
{
    if (!D.open)
        return 0;
    int s = D.sel[D.tab];
    if (p & PSP_CTRL_CIRCLE) {
        D.open = 0;
        spring_to(&D.t, 0);
    } else if (p & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) {
        int t = p & PSP_CTRL_LTRIGGER ? TAB_SAVES : TAB_BACKUPS;
        if (t != D.tab) {
            D.tab = t;
            sfx_play(SFX_CARD);
            select_row(D.sel[t], 1);
        }
    } else if (p & PSP_CTRL_UP) select_row(s - 1, 0);
    else if (p & PSP_CTRL_DOWN) select_row(s + 1, 0);
    else if (p & PSP_CTRL_LEFT) select_row(s - ROWS, 0);
    else if (p & PSP_CTRL_RIGHT) select_row(s + ROWS, 0);
    else if (p & (PSP_CTRL_TRIANGLE | PSP_CTRL_CROSS)) {
        if (J.kind != J_NONE) ui_toast("Wait for this one to finish");
        else open_menu();
    }
    return 1;
}

int sd_step(void)
{
    int dirty = 0, old = D.t.x;
    spring_step(&D.t);
    dirty |= D.t.x != old;
    old = D.hl.x; spring_step(&D.hl); dirty |= D.hl.x != old;
    old = D.scroll.x; spring_step(&D.scroll); dirty |= D.scroll.x != old;
    if (R.ready) { /* a scan finished: take its lists */
        R.ready = 0;
        SceUInt to = 1000000;
        sceKernelWaitThreadEnd(s_thread, &to);
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
        for (int t = 0; t < TABS; t++) {
            free(D.items[t]);
            D.items[t] = R.items[t];
            D.n[t] = R.n[t];
            R.items[t] = NULL;
        }
        D.loading = 0;
        int t = D.tab;
        D.tab = TAB_SAVES; select_row(D.sel[TAB_SAVES], 1);
        D.tab = TAB_BACKUPS; select_row(D.sel[TAB_BACKUPS], 1);
        D.tab = t; select_row(D.sel[t], 1);
        dirty = 1;
    }
    dirty |= finish_job();
    static int tick;
    if (J.kind != J_NONE && D.open && (++tick & 7) == 0)
        dirty = 1; /* the progress bar */
    return dirty;
}

/* ------------------------------------------------------------------ draw */

static void draw_tabs(int y)
{
    static const char *const names[TABS] = {"Saves", "Backups"};
    int x = 12;
    for (int t = 0; t < TABS; t++) {
        char label[32];
        snprintf(label, sizeof(label), D.loading ? "%s" : "%s  %d", names[t], D.n[t]);
        int w = gfx_text_width(FONT_SMALL, label) + 20, h = gfx_line_h(FONT_SMALL) + 6;
        gfx_rrect(x, y, w, h, h / 2, t == D.tab ? C_ACCENT : C_RAISED);
        gfx_text(FONT_SMALL, x + 10, y + 3, label, t == D.tab ? C_WHITE : C_MUTED);
        x += w + 6;
    }
}

static void draw_row(const SdItem *s, int y, int focused)
{
    const int x = 12, w = SCREEN_W - 24;
    const void *tex = s->has_icon ? thumbs_get(&s->icon) : NULL;
    int iy = y + (ROW_H - ICON_H) / 2;
    if (tex) {
        gfx_thumb(tex, x + 4, iy, ICON_W, ICON_H);
        gfx_corners(x + 4, iy, ICON_W, ICON_H, 4, focused ? C_ACCENT : C_BG);
    } else {
        gfx_rrect(x + 4, iy, ICON_W, ICON_H, 4, focused ? C_ACCENT_DK : C_SURFACE);
        gfx_icon(ICON_GAMES, x + 4 + (ICON_W - 20) / 2, iy + (ICON_H - 20) / 2, 20, 20, C_MUTED);
    }
    int tx = x + ICON_W + 14, tw = x + w - 8 - tx;
    char line[200], size[16], date[32];
    format_size(size, sizeof(size), s->size);
    gfx_text_right(FONT_SMALL, x + w - 8, y + 4, size, focused ? C_WHITE : C_MUTED);
    gfx_text_fit(FONT_BOLD, tx, y + 2, tw - gfx_text_width(FONT_SMALL, size) - 10, s->title, focused ? C_WHITE : C_TEXT);
    if (s->day[0]) snprintf(date, sizeof(date), "backed up %s", s->day);
    else format_date(date, sizeof(date), s->mtime);
    snprintf(line, sizeof(line), s->sub[0] ? "%s" DOT "%s" : "%s%s", s->sub[0] ? s->sub : "", date);
    gfx_text_fit(FONT_SMALL, tx, y + 20, tw, line, focused ? C_WHITE : C_MUTED);
}

void sd_draw(const SysStatus *st)
{
    int t = D.t.x;
    if (t <= 0)
        return;
    if (t > 256) t = 256;
    gfx_set_opacity(t);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
    ui_draw_status_bar("Save data", st);
    draw_tabs(26);

    int n = D.n[D.tab], bottom = ROW_Y + ROWS * ROW_H;
    if (D.loading && !D.items[D.tab]) {
        gfx_text_center(FONT_BODY, SCREEN_W / 2, 140, "Reading the saves...", C_MUTED);
    } else if (n == 0) {
        gfx_text_center(FONT_BODY, SCREEN_W / 2, 130, D.tab == TAB_SAVES ? "No saves in PSP/SAVEDATA" : "No backups yet", C_MUTED);
        if (D.tab == TAB_BACKUPS)
            gfx_text_center(FONT_SMALL, SCREEN_W / 2, 154, "Triangle on a save, then Back up", C_MUTED);
    } else {
        int hy = ROW_Y + (((D.hl.x - D.scroll.x) * ROW_H) >> 8);
        gfx_rrect(12, hy, SCREEN_W - 24, ROW_H - 2, 6, C_ACCENT);
        int first = (D.scroll.x >> 8) - 1;
        for (int i = first < 0 ? 0 : first; i < n && i <= first + ROWS + 2; i++) {
            int y = ROW_Y + (((FX(i) - D.scroll.x) * ROW_H) >> 8);
            draw_row(&D.items[D.tab][i], y, i == D.sel[D.tab]);
        }
        /* rows scrolling past the list are covered */
        gfx_rect(0, 0, SCREEN_W, ROW_Y, C_BG);
        gfx_rect(0, bottom, SCREEN_W, SCREEN_H - bottom, C_BG);
        ui_draw_status_bar("Save data", st);
        draw_tabs(26);
        if (n > ROWS) { /* where in the list */
            int h = bottom - ROW_Y, bh = h * ROWS / n;
            if (bh < 10) bh = 10;
            int y = ROW_Y + (h - bh) * D.top[D.tab] / (n - ROWS);
            gfx_rrect(SCREEN_W - 6, y, 3, bh, 1, C_RAISED);
        }
    }

    gfx_rect(0, 248, SCREEN_W, 1, C_LINE);
    if (J.kind != J_NONE) { /* the job, instead of the hints */
        FopStatus fs;
        fop_status(&fs);
        int pct = fs.bytes_total ? (int)(fs.bytes_done * 100 / fs.bytes_total) : 0;
        char m[64];
        snprintf(m, sizeof(m), "%s... %d%%", J.kind == J_BACKUP ? "Backing up" : J.kind == J_DELETE ? "Deleting"
                                             : J.kind == J_RESTORE_CLEAR ? "Clearing the old save" : "Restoring", pct);
        gfx_text(FONT_SMALL, 12, 253, m, C_TEXT);
        int bx = 200, bw = SCREEN_W - 12 - bx;
        gfx_rrect(bx, 259, bw, 4, 2, C_RAISED);
        if (pct * bw / 100 >= 4) gfx_rrect(bx, 259, pct * bw / 100, 4, 2, C_FOCUS);
    } else {
        static const Hint h[] = {
            {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Options"},
            {-1, "L/R", 0, "Saves / Backups"},
            {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
        };
        ui_draw_hints(h, COUNT(h), SCREEN_W / 2, 253);
    }
    gfx_set_opacity(256);
}
