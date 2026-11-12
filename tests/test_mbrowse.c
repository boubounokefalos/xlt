/* Music: the Artists and Albums lists (mbrowse.c). */
#include <stdio.h>
#include <string.h>
#include "mbrowse.h"

static Entry s_e[16];
static int s_n;
static const Entry *get(int i) { return &s_e[i]; }
static int fails;
#define CHECK(what, cond) do { int ok_ = (cond); printf("%s  %s\n", ok_ ? "OK  " : "FAIL", what); fails += !ok_; } while (0)

static void add(const char *path, const char *artist, const char *album, int track, const char *title)
{
    Entry *e = &s_e[s_n++];
    memset(e, 0, sizeof(*e));
    snprintf(e->path, sizeof(e->path), "%s", path);
    snprintf(e->artist, sizeof(e->artist), "%s", artist);
    snprintf(e->album, sizeof(e->album), "%s", album);
    snprintf(e->title, sizeof(e->title), "%s", title);
    e->track = track;
    e->kind = KIND_MUSIC;
}

static int find(int kind, const char *name, const char *sub)
{
    for (int k = 0; k < mb_count(kind); k++)
        if (!strcmp(mb_group(kind, k)->name, name) && (!sub || !strcmp(mb_group(kind, k)->sub, sub)))
            return k;
    return -1;
}

int main(void)
{
    /* Not in a helpful order on purpose: the library may be sorted by file name. */
    add("ms0:/MUSIC/Queen/GH/02.mp3", "Queen", "Greatest Hits", 2, "Another One");
    add("ms0:/MUSIC/ABBA/GH/01.mp3", "ABBA", "Greatest Hits", 1, "SOS");
    add("ms0:/MUSIC/Queen/GH/01.mp3", "queen", "Greatest Hits", 1, "Bohemian");
    add("ms0:/MUSIC/Mix/1.mp3", "Daft Punk", "Summer Mix", 1, "One");
    add("ms0:/MUSIC/Mix/2.mp3", "Air", "Summer Mix", 2, "Two");
    add("ms0:/MUSIC/Queen/Live/10.mp3", "Queen", "Live", 10, "Ten");
    add("ms0:/MUSIC/Queen/Live/2.mp3", "Queen", "Live", 2, "Two");
    add("ms0:/MUSIC/Loose/track.mp3", "", "", 0, "track");
    add("ms0:/MUSIC/Loose/other.mp3", "", "", 0, "other");
    add("ms0:/MUSIC/memo.mp3", "", "", 0, "memo");
    CHECK("builds", mb_build(get, s_n) == 0);

    CHECK("5 artists (case ignored, unknown counted)", mb_count(MB_ARTISTS) == 5);
    CHECK("artists by name, unknown last", !strcmp(mb_group(MB_ARTISTS, 0)->name, "ABBA") &&
                                              !strcmp(mb_group(MB_ARTISTS, 3)->name, "Queen") &&
                                              mb_group(MB_ARTISTS, 4)->name[0] == 0);
    int q = find(MB_ARTISTS, "Queen", NULL);
    CHECK("Queen: 4 songs on 2 albums", q >= 0 && mb_group(MB_ARTISTS, q)->count == 4 && mb_group(MB_ARTISTS, q)->albums == 2);

    CHECK("6 albums", mb_count(MB_ALBUMS) == 6);
    CHECK("loose songs in MUSIC/: an unnamed album, last", mb_group(MB_ALBUMS, 5)->name[0] == 0 && mb_group(MB_ALBUMS, 5)->count == 1);
    int qg = find(MB_ALBUMS, "Greatest Hits", "Queen"), ag = find(MB_ALBUMS, "Greatest Hits", "ABBA");
    CHECK("two Greatest Hits stay apart", qg >= 0 && ag >= 0 && qg != ag);
    int mix = find(MB_ALBUMS, "Summer Mix", NULL);
    CHECK("a compilation stays one album, Various artists",
          mix >= 0 && mb_group(MB_ALBUMS, mix)->count == 2 && !strcmp(mb_group(MB_ALBUMS, mix)->sub, "Various artists"));
    int lo = find(MB_ALBUMS, "Loose", NULL);
    CHECK("untagged songs: an album named after their folder", lo >= 0 && mb_group(MB_ALBUMS, lo)->count == 2);

    int idx[16], n = mb_songs(MB_ALBUMS, find(MB_ALBUMS, "Live", NULL), idx, 16);
    CHECK("album songs by track (2 before 10)", n == 2 && s_e[idx[0]].track == 2 && s_e[idx[1]].track == 10);
    n = mb_songs(MB_ARTISTS, q, idx, 16);
    CHECK("artist songs by album then track",
          n == 4 && !strcmp(s_e[idx[0]].title, "Bohemian") && !strcmp(s_e[idx[1]].title, "Another One") &&
              s_e[idx[2]].track == 2 && s_e[idx[3]].track == 10);
    CHECK("max is respected", mb_songs(MB_ARTISTS, q, idx, 3) == 3);
    CHECK("group_of agrees", mb_group_of(MB_ARTISTS, 5) == q && mb_group_of(MB_ALBUMS, 3) == mix);
    CHECK("unknown artist has 3 songs on 2 albums", mb_group(MB_ARTISTS, 4)->count == 3 && mb_group(MB_ARTISTS, 4)->albums == 2);
    CHECK("bad indices", !mb_group(MB_ALBUMS, 99) && mb_group_of(MB_ALBUMS, -1) == -1 && mb_songs(MB_ALBUMS, 99, idx, 16) == 0);

    CHECK("empty library", mb_build(get, 0) == 0 && mb_count(MB_ARTISTS) == 0 && mb_count(MB_ALBUMS) == 0);
    mb_free();
    printf(fails ? "test_mbrowse: %d failed\n" : "test_mbrowse: ok\n", fails);
    return fails != 0;
}
