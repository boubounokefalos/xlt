/* XLT - playlists. See playlists.h.
 *
 * The list of playlists is reread on the background worker (a folder walk
 * and a quick count of each file). Edits change the list here at once and
 * queue the write; every read of a playlist file first waits for the queued
 * writes (bg_flush), so it never sees an old copy. A reread that was started
 * before an edit is thrown away and done again.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "playlists.h"
#include "m3u.h"
#include "musicsort.h"
#include "library.h"
#include "music.h"
#include "bg.h"

#define FILE_MAX (512 * 1024) /* a playlist bigger than this is read in part */

typedef struct {
    char path[M3U_PATH];
    char name[PL_NAME];
    int songs;
} Pl;

typedef struct {
    char path[M3U_PATH];
    char title[128];
} Song;

/* UI thread */
static Pl s_pl[PL_MAX];
static int s_n;
static int s_edits;          /* bumped by every change made here */
static int s_scan_posted, s_scan_again;
static char s_open_path[M3U_PATH]; /* "" = none open */
static Song *s_songs;        /* the open playlist, as in its file */
static Entry *s_items;       /* the same songs, as library entries */
static int s_items_n;

/* worker -> UI */
static struct {
    volatile int ready;
    int edits;
    Pl pl[PL_MAX];
    int n;
} R;
static volatile int s_save_err;

/* ------------------------------------------------------------- files */

/* The whole file in a malloc'd, NUL-terminated buffer (NULL if unreadable). */
static char *read_all(const char *path, int *len)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return NULL;
    int size = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    if (size < 0) size = 0;
    if (size > FILE_MAX) size = FILE_MAX;
    char *b = malloc(size + 1);
    int n = b ? sceIoRead(fd, b, size) : -1;
    sceIoClose(fd);
    if (n < 0) {
        free(b);
        return NULL;
    }
    b[n] = 0;
    *len = n;
    return b;
}

static int by_name(const void *a, const void *b)
{
    const Pl *x = a, *y = b;
    int r = natural_cmp(x->name, y->name);
    return r ? r : strcasecmp(x->path, y->path);
}

static void name_of(const char *path, char *out, int cap)
{
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    const char *dot = strrchr(b, '.');
    snprintf(out, cap, "%.*s", dot ? (int)(dot - b) : (int)strlen(b), b);
}

/* ------------------------------------------------------- the reread */

static void job_scan(void *arg)
{
    static const char *const roots[] = {"ms0:", "ef0:"};
    static const char *const dirs[] = {"MUSIC", "MUSIC/PLAYLISTS"};
    int edits = *(const int *)arg, n = 0;
    char dir[64], path[M3U_PATH];
    for (int r = 0; r < 2; r++)
        for (int d = 0; d < 2; d++) {
            snprintf(dir, sizeof(dir), "%s/%s", roots[r], dirs[d]);
            SceUID dh = sceIoDopen(dir);
            if (dh < 0)
                continue;
            SceIoDirent de;
            memset(&de, 0, sizeof(de));
            while (n < PL_MAX && sceIoDread(dh, &de) > 0) {
                if (!FIO_S_ISDIR(de.d_stat.st_mode) && de.d_name[0] != '.' && m3u_is_playlist(de.d_name) &&
                    snprintf(path, sizeof(path), "%s/%s", dir, de.d_name) < (int)sizeof(path)) {
                    Pl *p = &R.pl[n++];
                    snprintf(p->path, sizeof(p->path), "%s", path);
                    name_of(path, p->name, sizeof(p->name));
                    int len = 0;
                    char *text = read_all(path, &len);
                    p->songs = text ? m3u_parse(text, len, path, NULL, NULL) : 0;
                    free(text);
                }
                memset(&de, 0, sizeof(de));
            }
            sceIoDclose(dh);
        }
    qsort(R.pl, n, sizeof(Pl), by_name);
    R.n = n;
    R.edits = edits;
    R.ready = 1;
}

void pl_refresh(void)
{
    if (s_scan_posted) { /* one at a time: R is the worker's until it is adopted */
        s_scan_again = 1;
        return;
    }
    s_scan_posted = 1;
    s_scan_again = 0;
    bg_post(job_scan, &s_edits, sizeof(s_edits));
}

int pl_poll(int *err)
{
    if (err) {
        *err = s_save_err;
        s_save_err = 0;
    }
    if (!R.ready)
        return 0;
    int fresh = R.edits == s_edits; /* else it was started before an edit */
    if (fresh) {
        memcpy(s_pl, R.pl, sizeof(Pl) * R.n);
        s_n = R.n;
    }
    R.ready = 0;
    s_scan_posted = 0;
    if (!fresh || s_scan_again)
        pl_refresh();
    return fresh;
}

int pl_count(void) { return s_n; }
const char *pl_name(int i) { return i >= 0 && i < s_n ? s_pl[i].name : ""; }
const char *pl_path(int i) { return i >= 0 && i < s_n ? s_pl[i].path : ""; }
int pl_songs(int i) { return i >= 0 && i < s_n ? s_pl[i].songs : 0; }

static int find_path(const char *path)
{
    for (int i = 0; i < s_n; i++)
        if (!strcasecmp(s_pl[i].path, path))
            return i;
    return -1;
}

/* After the list was changed here: in order again, and a reread in flight is stale. */
static void changed(void)
{
    qsort(s_pl, s_n, sizeof(Pl), by_name);
    s_edits++;
}

/* --------------------------------------------------------- saving */

typedef struct {
    char *text;
    int len;
    char path[M3U_PATH];
} WriteArg;

static void job_write(void *arg)
{
    const WriteArg *a = arg;
    char dir[M3U_PATH];
    /* MUSIC/ and MUSIC/PLAYLISTS/ may not exist yet */
    for (const char *s = strchr(a->path, '/'); s; s = strchr(s + 1, '/')) {
        int n = (int)(s - a->path);
        if (n > 0 && a->path[n - 1] != ':') {
            snprintf(dir, sizeof(dir), "%.*s", n, a->path);
            sceIoMkdir(dir, 0777);
        }
    }
    SceUID fd = sceIoOpen(a->path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0 || sceIoWrite(fd, a->text, a->len) != a->len)
        s_save_err = 1;
    if (fd >= 0)
        sceIoClose(fd);
    free(a->text);
}

/* Queues the write of songs[0..n) to path. */
static int save(const char *path, const Song *songs, int n)
{
    const char **paths = malloc(sizeof(char *) * (n ? n : 1)), **titles = malloc(sizeof(char *) * (n ? n : 1));
    int cap = 64 + n * (M3U_PATH + 140);
    char *text = malloc(cap + 3);
    if (!paths || !titles || !text) {
        free(paths); free(titles); free(text);
        return PL_ERR_MEM;
    }
    for (int i = 0; i < n; i++) {
        paths[i] = songs[i].path;
        titles[i] = songs[i].title;
    }
    /* a plain .m3u gets a UTF-8 mark, or players read it as Latin-1 */
    int n0 = strcasecmp(path + strlen(path) - 5, ".m3u8") ? 3 : 0;
    memcpy(text, "\xEF\xBB\xBF", 3);
    int len = m3u_write(text + n0, cap, path, paths, titles, n);
    free(paths);
    free(titles);
    if (len < 0) {
        free(text);
        return PL_ERR_MEM;
    }
    WriteArg a;
    a.text = text;
    a.len = len + n0;
    snprintf(a.path, sizeof(a.path), "%s", path);
    bg_post(job_write, &a, sizeof(a)); /* the worker writes, then frees the text */
    return PL_OK;
}

/* --------------------------------------------------------- reading one */

typedef struct {
    Song *s;
    int n;
} Songs;

static void add_song(const char *path, const char *title, void *ctx)
{
    Songs *l = ctx;
    if (l->n >= PL_SONGS)
        return;
    snprintf(l->s[l->n].path, sizeof(l->s[0].path), "%s", path);
    snprintf(l->s[l->n].title, sizeof(l->s[0].title), "%s", title);
    l->n++;
}

/* The songs of the file at path (malloc'd, room for one more). */
static int load_songs(const char *path, Songs *out)
{
    out->n = 0;
    out->s = NULL;
    bg_flush(); /* a queued write of this file goes first */
    int len = 0;
    char *text = read_all(path, &len);
    if (!text)
        return PL_ERR_READ;
    int n = m3u_parse(text, len, path, NULL, NULL);
    if (n > PL_SONGS) n = PL_SONGS;
    out->s = malloc(sizeof(Song) * (n + 1));
    if (!out->s) {
        free(text);
        return PL_ERR_MEM;
    }
    m3u_parse(text, len, path, add_song, out);
    free(text);
    return PL_OK;
}

static void song_from(Song *s, const Entry *e)
{
    snprintf(s->path, sizeof(s->path), "%s", e->path);
    if (e->artist[0])
        snprintf(s->title, sizeof(s->title), "%.47s - %.76s", e->artist, e->title);
    else
        snprintf(s->title, sizeof(s->title), "%s", e->title);
}

/* Library lookups by path, ignoring case (FAT does, and playlists are typed by people). */
static int *s_lib;
static int s_lib_n;
static int lib_cmp(const void *a, const void *b)
{
    return strcasecmp(library_get(VIEW_MUSIC, *(const int *)a)->path, library_get(VIEW_MUSIC, *(const int *)b)->path);
}

static void lib_index(void)
{
    free(s_lib);
    s_lib_n = library_count(VIEW_MUSIC);
    s_lib = malloc(sizeof(int) * (s_lib_n ? s_lib_n : 1));
    if (!s_lib) {
        s_lib_n = 0;
        return;
    }
    for (int i = 0; i < s_lib_n; i++)
        s_lib[i] = i;
    qsort(s_lib, s_lib_n, sizeof(int), lib_cmp);
}

static const Entry *lib_find(const char *path)
{
    int lo = 0, hi = s_lib_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const Entry *e = library_get(VIEW_MUSIC, s_lib[mid]);
        int r = strcasecmp(path, e->path);
        if (!r) return e;
        if (r < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

static void item_from(Entry *e, const Song *s)
{
    const Entry *lib = lib_find(s->path);
    if (lib) {
        *e = *lib;
        return;
    }
    memset(e, 0, sizeof(*e)); /* not in the library: its name, or the playlist's title */
    snprintf(e->path, sizeof(e->path), "%s", s->path);
    if (s->title[0])
        snprintf(e->title, sizeof(e->title), "%s", s->title);
    else
        name_of(s->path, e->title, sizeof(e->title));
    e->kind = KIND_MUSIC;
}

void pl_close(void)
{
    s_open_path[0] = 0;
    free(s_songs);
    free(s_items);
    s_songs = NULL;
    s_items = NULL;
    s_items_n = 0;
}

int pl_open(int i)
{
    pl_close();
    if (i < 0 || i >= s_n)
        return PL_ERR_READ;
    Songs l;
    int r = load_songs(s_pl[i].path, &l);
    if (r != PL_OK)
        return r;
    s_items = malloc(sizeof(Entry) * (l.n + 1));
    if (!s_items) {
        free(l.s);
        return PL_ERR_MEM;
    }
    lib_index();
    for (int k = 0; k < l.n; k++)
        item_from(&s_items[k], &l.s[k]);
    free(s_lib);
    s_lib = NULL;
    s_lib_n = 0;
    s_songs = l.s;
    s_items_n = l.n;
    s_pl[i].songs = l.n;
    snprintf(s_open_path, sizeof(s_open_path), "%s", s_pl[i].path);
    return PL_OK;
}

int pl_open_index(void) { return s_open_path[0] ? find_path(s_open_path) : -1; }
int pl_item_count(void) { return s_items_n; }
const Entry *pl_item(int k) { return k >= 0 && k < s_items_n ? &s_items[k] : NULL; }

void pl_play(int k)
{
    if (k >= 0 && k < s_items_n)
        music_play_entries(s_items, s_items_n, k); /* the player copies the list */
}

int pl_remove_item(int k)
{
    int i = pl_open_index();
    if (i < 0 || k < 0 || k >= s_items_n)
        return PL_ERR_READ;
    memmove(&s_songs[k], &s_songs[k + 1], sizeof(Song) * (s_items_n - k - 1));
    memmove(&s_items[k], &s_items[k + 1], sizeof(Entry) * (s_items_n - k - 1));
    s_items_n--;
    s_pl[i].songs = s_items_n;
    s_edits++;
    return save(s_open_path, s_songs, s_items_n);
}

int pl_move_item(int k, int dir)
{
    int j = k + dir;
    if (k < 0 || k >= s_items_n || j < 0 || j >= s_items_n)
        return -1;
    Song ts = s_songs[k];
    s_songs[k] = s_songs[j];
    s_songs[j] = ts;
    Entry te = s_items[k];
    s_items[k] = s_items[j];
    s_items[j] = te;
    return j;
}

int pl_save_open(void)
{
    if (pl_open_index() < 0)
        return PL_ERR_READ;
    s_edits++;
    return save(s_open_path, s_songs, s_items_n);
}

/* ------------------------------------------------------------ edits */

int pl_add(int i, const Entry *e)
{
    if (i < 0 || i >= s_n)
        return PL_ERR_READ;
    if (i == pl_open_index()) { /* the one on screen: its songs are here already */
        for (int k = 0; k < s_items_n; k++)
            if (!strcasecmp(s_songs[k].path, e->path))
                return 1;
        if (s_items_n >= PL_SONGS)
            return PL_ERR_FULL;
        Song *ns = realloc(s_songs, sizeof(Song) * (s_items_n + 1));
        if (ns) s_songs = ns;
        Entry *ni = realloc(s_items, sizeof(Entry) * (s_items_n + 1));
        if (ni) s_items = ni;
        if (!ns || !ni)
            return PL_ERR_MEM;
        song_from(&s_songs[s_items_n], e);
        s_items[s_items_n++] = *e;
        s_pl[i].songs = s_items_n;
        s_edits++;
        return save(s_open_path, s_songs, s_items_n);
    }
    Songs l;
    int r = load_songs(s_pl[i].path, &l);
    if (r != PL_OK)
        return r;
    for (int k = 0; k < l.n; k++)
        if (!strcasecmp(l.s[k].path, e->path)) {
            free(l.s);
            return 1;
        }
    if (l.n >= PL_SONGS) {
        free(l.s);
        return PL_ERR_FULL;
    }
    song_from(&l.s[l.n++], e); /* load_songs left room for one more */
    s_pl[i].songs = l.n;
    s_edits++;
    r = save(s_pl[i].path, l.s, l.n);
    free(l.s);
    return r;
}

int pl_add_many(int i, const Entry *list, int n, int *added)
{
    *added = 0;
    if (i < 0 || i >= s_n)
        return PL_ERR_READ;
    int open = i == pl_open_index();
    Songs l = {NULL, 0};
    if (open) { /* the one on screen: its songs are here already */
        l.s = malloc(sizeof(Song) * (s_items_n + n));
        if (!l.s)
            return PL_ERR_MEM;
        memcpy(l.s, s_songs, sizeof(Song) * s_items_n);
        l.n = s_items_n;
    } else {
        int r = load_songs(s_pl[i].path, &l);
        if (r != PL_OK)
            return r;
        Song *ns = realloc(l.s, sizeof(Song) * (l.n + n + 1));
        if (!ns) {
            free(l.s);
            return PL_ERR_MEM;
        }
        l.s = ns;
    }
    int full = 0;
    if (open) { /* room first, so the open list and its songs always stay the same length */
        Song *ns = realloc(s_songs, sizeof(Song) * (s_items_n + n));
        if (ns) s_songs = ns;
        Entry *ni = ns ? realloc(s_items, sizeof(Entry) * (s_items_n + n)) : NULL;
        if (ni) s_items = ni;
        if (!ns || !ni) {
            free(l.s);
            return PL_ERR_MEM;
        }
    }
    for (int k = 0; k < n; k++) {
        int dup = 0;
        for (int j = 0; j < l.n && !dup; j++)
            dup = !strcasecmp(l.s[j].path, list[k].path);
        if (dup)
            continue;
        if (l.n >= PL_SONGS) {
            full = 1;
            break;
        }
        song_from(&l.s[l.n], &list[k]);
        if (open) {
            s_songs[s_items_n] = l.s[l.n];
            s_items[s_items_n++] = list[k];
        }
        l.n++;
        (*added)++;
    }
    int r = PL_OK;
    if (*added) {
        s_pl[i].songs = l.n;
        s_edits++;
        r = save(open ? s_open_path : s_pl[i].path, l.s, l.n);
    }
    free(l.s);
    return r != PL_OK ? r : full ? PL_ERR_FULL : PL_OK;
}

/* A clean name for a file: no surrounding spaces, not empty. */
static int clean_name(const char *in, char *out, int cap)
{
    while (*in == ' ') in++;
    int n = (int)strlen(in);
    while (n > 0 && (in[n - 1] == ' ' || in[n - 1] == '.')) n--;
    if (n <= 0)
        return PL_ERR_NAME;
    if (n >= cap) n = cap - 1;
    snprintf(out, cap, "%.*s", n, in);
    return PL_OK;
}

static int file_exists(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

/* 1 if a playlist called name is in the folder dir already (either extension). */
static int name_taken(const char *dir, const char *name, int except)
{
    char p[M3U_PATH];
    for (int i = 0; i < s_n; i++) {
        const char *sl = strrchr(s_pl[i].path, '/');
        int dl = sl ? (int)(sl - s_pl[i].path) : 0;
        if (i != except && (int)strlen(dir) == dl && !strncasecmp(dir, s_pl[i].path, dl) && !strcasecmp(s_pl[i].name, name))
            return 1;
    }
    snprintf(p, sizeof(p), "%s/%s.m3u8", dir, name);
    if (file_exists(p)) return 1;
    snprintf(p, sizeof(p), "%s/%s.m3u", dir, name);
    return file_exists(p);
}

int pl_create(const char *name_in, const Entry *song)
{
    char name[PL_NAME], dir[64], path[M3U_PATH];
    if (clean_name(name_in, name, 50) != PL_OK)
        return PL_ERR_NAME;
    if (s_n >= PL_MAX)
        return PL_ERR_FULL;
    const char *c = song ? strchr(song->path, ':') : NULL;
    snprintf(dir, sizeof(dir), "%.*s/MUSIC/PLAYLISTS", c && c - song->path < 8 ? (int)(c - song->path + 1) : 4,
             c && c - song->path < 8 ? song->path : "ms0:");
    bg_flush();
    if (name_taken(dir, name, -1))
        return PL_ERR_EXISTS;
    if (snprintf(path, sizeof(path), "%s/%s.m3u8", dir, name) >= (int)sizeof(path))
        return PL_ERR_NAME;
    Song s;
    if (song)
        song_from(&s, song);
    int r = save(path, &s, song ? 1 : 0);
    if (r != PL_OK)
        return r;
    Pl *p = &s_pl[s_n++];
    snprintf(p->path, sizeof(p->path), "%s", path);
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->songs = song ? 1 : 0;
    changed();
    return find_path(path);
}

typedef struct {
    char from[M3U_PATH], to[M3U_PATH];
} RenameArg;

static void job_rename(void *arg)
{
    const RenameArg *a = arg;
    if (sceIoRename(a->from, a->to) < 0)
        s_save_err = 1;
}

int pl_rename(int i, const char *name_in)
{
    char name[PL_NAME], dir[M3U_PATH];
    if (i < 0 || i >= s_n)
        return PL_ERR_READ;
    if (clean_name(name_in, name, 50) != PL_OK)
        return PL_ERR_NAME;
    if (!strcmp(name, s_pl[i].name))
        return i;
    const char *sl = strrchr(s_pl[i].path, '/'), *ext = strrchr(s_pl[i].path, '.');
    snprintf(dir, sizeof(dir), "%.*s", sl ? (int)(sl - s_pl[i].path) : 0, s_pl[i].path);
    bg_flush();
    if (strcasecmp(name, s_pl[i].name) && name_taken(dir, name, i)) /* only the case changes: it's the same file */
        return PL_ERR_EXISTS;
    RenameArg a;
    snprintf(a.from, sizeof(a.from), "%s", s_pl[i].path);
    if (snprintf(a.to, sizeof(a.to), "%s/%s%s", dir, name, ext ? ext : ".m3u8") >= (int)sizeof(a.to))
        return PL_ERR_NAME;
    bg_post(job_rename, &a, sizeof(a));
    int was_open = !strcasecmp(s_open_path, s_pl[i].path);
    snprintf(s_pl[i].path, sizeof(s_pl[i].path), "%s", a.to);
    snprintf(s_pl[i].name, sizeof(s_pl[i].name), "%s", name);
    if (was_open)
        snprintf(s_open_path, sizeof(s_open_path), "%s", a.to);
    changed();
    return find_path(a.to);
}

typedef struct {
    char path[M3U_PATH];
} RemoveArg;

static void job_remove(void *arg)
{
    const RemoveArg *a = arg;
    if (sceIoRemove(a->path) < 0)
        s_save_err = 1;
}

void pl_delete(int i)
{
    if (i < 0 || i >= s_n)
        return;
    RemoveArg a;
    snprintf(a.path, sizeof(a.path), "%s", s_pl[i].path);
    bg_post(job_remove, &a, sizeof(a));
    if (!strcasecmp(s_open_path, s_pl[i].path))
        pl_close();
    memmove(&s_pl[i], &s_pl[i + 1], sizeof(Pl) * (s_n - i - 1));
    s_n--;
    s_edits++;
}
