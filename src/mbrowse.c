/* XLT - Music: the Artists and Albums lists. See mbrowse.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "mbrowse.h"
#include "musicsort.h"

static const Entry *(*s_get)(int);
static int s_n;
static int *s_of[MB_KINDS];   /* per song: its group */
static MbGroup *s_g[MB_KINDS];
static int s_gn[MB_KINDS];

void mb_free(void)
{
    for (int k = 0; k < MB_KINDS; k++) {
        free(s_of[k]);
        free(s_g[k]);
        s_of[k] = NULL;
        s_g[k] = NULL;
        s_gn[k] = 0;
    }
    s_n = 0;
}

/* The folder part of a path ("ms0:/MUSIC/A/B" for ".../B/song.mp3"). */
static int dir_len(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? (int)(s - p) : 0;
}

static const char *dir_name(const char *p, char *out, int n)
{
    int l = dir_len(p);
    int a = l;
    while (a > 0 && p[a - 1] != '/')
        a--;
    snprintf(out, n, "%.*s", l - a, p + a);
    if (!memchr(p, '/', a > 0 ? a - 1 : 0) && !strcasecmp(out, "MUSIC"))
        out[0] = 0; /* loose songs in MUSIC/ itself: no album to name after */
    return out;
}

/* Song i's key for each kind: compared to find the song's group. */
static int same_group(int kind, const Entry *a, const Entry *b)
{
    if (kind == MB_ARTISTS)
        return !strcasecmp(a->artist, b->artist);
    int la = dir_len(a->path), lb = dir_len(b->path);
    return la == lb && !strncmp(a->path, b->path, la) && !strcasecmp(a->album, b->album);
}

static int cmp_group(const void *x, const void *y)
{
    const MbGroup *a = x, *b = y;
    int ua = a->name[0] == 0, ub = b->name[0] == 0; /* untagged: last */
    if (ua != ub) return ua - ub;
    int r = natural_cmp(a->name, b->name);
    if (!r) r = natural_cmp(a->sub, b->sub);
    return r ? r : a->cover - b->cover;
}

/* The first song of each group stands for it while building (group.cover). */
static int build_kind(int kind)
{
    int *of = malloc(sizeof(int) * (s_n ? s_n : 1));
    MbGroup *g = malloc(sizeof(MbGroup) * (s_n ? s_n : 1));
    if (!of || !g) {
        free(of);
        free(g);
        return -1;
    }
    int gn = 0;
    for (int i = 0; i < s_n; i++) {
        const Entry *e = s_get(i);
        int k = 0;
        /* Songs of a group are usually next to each other in the library's order: look back first. */
        if (i > 0 && same_group(kind, e, s_get(i - 1)))
            k = of[i - 1];
        else {
            for (k = 0; k < gn && !same_group(kind, e, s_get(g[k].cover)); k++) {}
            if (k == gn) {
                memset(&g[gn], 0, sizeof(g[gn]));
                g[gn].cover = i;
                gn++;
            }
        }
        of[i] = k;
        g[k].count++;
    }
    /* Names. */
    for (int k = 0; k < gn; k++) {
        const Entry *e = s_get(g[k].cover);
        char d[MB_NAME];
        if (kind == MB_ARTISTS)
            snprintf(g[k].name, MB_NAME, "%s", e->artist);
        else
            snprintf(g[k].name, MB_NAME, "%s", e->album[0] ? e->album : dir_name(e->path, d, sizeof(d)));
    }
    if (kind == MB_ALBUMS)
        for (int i = 0; i < s_n; i++) { /* the album's artist, or "Various artists" */
            MbGroup *a = &g[of[i]];
            const char *ar = s_get(i)->artist;
            if (!ar[0]) continue;
            if (!a->sub[0]) snprintf(a->sub, MB_NAME, "%s", ar);
            else if (strcasecmp(a->sub, ar) && strcmp(a->sub, "Various artists")) snprintf(a->sub, MB_NAME, "Various artists");
        }
    /* Sort the groups by name and renumber the songs. */
    int *rank = malloc(sizeof(int) * (gn ? gn : 1));
    if (!rank) {
        free(of);
        free(g);
        return -1;
    }
    typedef struct { MbGroup g; int old; } Pair;
    Pair *p = malloc(sizeof(Pair) * (gn ? gn : 1));
    if (!p) {
        free(rank);
        free(of);
        free(g);
        return -1;
    }
    for (int k = 0; k < gn; k++) {
        p[k].g = g[k];
        p[k].old = k;
    }
    qsort(p, gn, sizeof(Pair), cmp_group);
    for (int k = 0; k < gn; k++) {
        g[k] = p[k].g;
        rank[p[k].old] = k;
    }
    for (int i = 0; i < s_n; i++)
        of[i] = rank[of[i]];
    free(p);
    free(rank);
    s_of[kind] = of;
    s_g[kind] = g;
    s_gn[kind] = gn;
    return 0;
}

static int cmp_ll(const void *x, const void *y)
{
    long long a = *(const long long *)x, b = *(const long long *)y;
    return a < b ? -1 : a > b;
}

int mb_build(const Entry *(*get)(int i), int n)
{
    mb_free();
    s_get = get;
    s_n = n > 0 ? n : 0;
    for (int k = 0; k < MB_KINDS; k++)
        if (build_kind(k) < 0) {
            mb_free();
            return -1;
        }
    /* How many albums each artist has: the distinct (artist, album) pairs. */
    long long *pair = malloc(sizeof(long long) * (s_n ? s_n : 1));
    if (pair) {
        for (int i = 0; i < s_n; i++)
            pair[i] = (long long)s_of[MB_ARTISTS][i] << 32 | (unsigned)s_of[MB_ALBUMS][i];
        qsort(pair, s_n, sizeof(long long), cmp_ll);
        for (int i = 0; i < s_n; i++)
            if (i == 0 || pair[i] != pair[i - 1])
                s_g[MB_ARTISTS][pair[i] >> 32].albums++;
        free(pair);
    }
    return 0;
}

int mb_count(int kind) { return kind >= 0 && kind < MB_KINDS ? s_gn[kind] : 0; }

const MbGroup *mb_group(int kind, int k)
{
    return kind >= 0 && kind < MB_KINDS && k >= 0 && k < s_gn[kind] ? &s_g[kind][k] : NULL;
}

int mb_group_of(int kind, int i)
{
    return kind >= 0 && kind < MB_KINDS && s_of[kind] && i >= 0 && i < s_n ? s_of[kind][i] : -1;
}

static int s_cmp_kind;
static int cmp_song(const void *x, const void *y)
{
    const Entry *a = s_get(*(const int *)x), *b = s_get(*(const int *)y);
    return msort_cmp(a, b, s_cmp_kind == MB_ARTISTS ? MSORT_ARTIST : MSORT_ALBUM);
}

int mb_songs(int kind, int k, int *out, int max)
{
    if (!mb_group(kind, k))
        return 0;
    int n = 0;
    for (int i = 0; i < s_n && n < max; i++)
        if (s_of[kind][i] == k)
            out[n++] = i;
    s_cmp_kind = kind;
    qsort(out, n, sizeof(int), cmp_song);
    return n;
}
