/* XLT - the order of the library lists. See musicsort.h. */
#include <string.h>
#include "musicsort.h"

static const char *const s_label[MSORT_COUNT] = {"Artist", "Album", "File name"};
static const char *const s_key[MSORT_COUNT] = {"artist", "album", "name"};

const char *msort_label(int m) { return s_label[m >= 0 && m < MSORT_COUNT ? m : 0]; }
const char *msort_key(int m) { return s_key[m >= 0 && m < MSORT_COUNT ? m : 0]; }

int msort_parse(const char *key)
{
    for (int i = 0; i < MSORT_COUNT; i++)
        if (!strcmp(key, s_key[i]))
            return i;
    return MSORT_ARTIST;
}

static int is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

int natural_cmp(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    while (*x && *y) {
        if (is_digit(*x) && is_digit(*y)) {
            /* two numbers: skip the leading zeros, then the longer one is bigger */
            while (*x == '0' && is_digit(x[1])) x++;
            while (*y == '0' && is_digit(y[1])) y++;
            int nx = 0, ny = 0;
            while (is_digit(x[nx])) nx++;
            while (is_digit(y[ny])) ny++;
            if (nx != ny)
                return nx < ny ? -1 : 1;
            int r = memcmp(x, y, nx);
            if (r)
                return r < 0 ? -1 : 1;
            x += nx;
            y += ny;
            continue;
        }
        int cx = lower(*x), cy = lower(*y);
        if (cx != cy)
            return cx < cy ? -1 : 1;
        x++;
        y++;
    }
    return *x ? 1 : *y ? -1 : 0;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Tagged before untagged; 0 if both or neither have the tag. */
static int tagged_first(const char *x, const char *y)
{
    if (!x[0] != !y[0])
        return x[0] ? -1 : 1;
    return 0;
}

static int by_track(const Entry *a, const Entry *b)
{
    if (a->track == b->track) return 0;
    if (!a->track || !b->track) return a->track ? -1 : 1; /* numbered tracks first */
    return a->track < b->track ? -1 : 1;
}

int msort_cmp(const Entry *a, const Entry *b, int mode)
{
    int r = 0;
    switch (mode) {
    case MSORT_ALBUM: /* album, its artist (two "Greatest Hits"), track, title */
        if ((r = tagged_first(a->album, b->album))) return r;
        if (!r) r = natural_cmp(a->album, b->album);
        if (!r) r = natural_cmp(a->artist, b->artist);
        if (!r) r = by_track(a, b);
        if (!r) r = natural_cmp(a->title, b->title);
        break;
    case MSORT_NAME:
        r = natural_cmp(base_name(a->path), base_name(b->path));
        break;
    default: /* artist, album, track, title */
        if ((r = tagged_first(a->artist, b->artist))) return r;
        if (!r) r = natural_cmp(a->artist, b->artist);
        if (!r) r = natural_cmp(a->album, b->album);
        if (!r) r = by_track(a, b);
        if (!r) r = natural_cmp(a->title, b->title);
        break;
    }
    return r ? r : strcmp(a->path, b->path);
}

/* ------------------------------------------------- Games and Homebrew */

static const char *const g_label[GSORT_COUNT] = {"Name", "Last played", "Size", "Newest"};
static const char *const g_key[GSORT_COUNT] = {"name", "played", "size", "newest"};

const char *gsort_label(int m) { return g_label[m >= 0 && m < GSORT_COUNT ? m : 0]; }
const char *gsort_key(int m) { return g_key[m >= 0 && m < GSORT_COUNT ? m : 0]; }

int gsort_parse(const char *key)
{
    for (int i = 0; i < GSORT_COUNT; i++)
        if (!strcmp(key, g_key[i]))
            return i;
    return GSORT_NAME;
}

int gsort_cmp(const Entry *a, const Entry *b, int mode, uint64_t played_a, uint64_t played_b)
{
    switch (mode) {
    case GSORT_PLAYED: /* most recent first */
        if (played_a != played_b) return played_a > played_b ? -1 : 1;
        break;
    case GSORT_SIZE: /* biggest first */
        if (a->size != b->size) return a->size > b->size ? -1 : 1;
        break;
    case GSORT_NEWEST:
        if (a->mtime != b->mtime) return a->mtime > b->mtime ? -1 : 1;
        break;
    }
    int r = natural_cmp(a->title, b->title);
    return r ? r : strcmp(a->path, b->path);
}
