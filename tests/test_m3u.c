/* m3u.c (reading, writing, paths) and musicsort.c (the Music list's order). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m3u.h"
#include "musicsort.h"

#define PL "ms0:/MUSIC/PLAYLISTS/Mix.m3u8"

typedef struct { int n; char path[32][M3U_PATH]; char title[32][128]; } Got;

static void got(const char *path, const char *title, void *ctx)
{
    Got *g = ctx;
    assert(g->n < 32);
    snprintf(g->path[g->n], M3U_PATH, "%s", path);
    snprintf(g->title[g->n], 128, "%s", title);
    g->n++;
}

static void expect_resolve(const char *line, const char *want)
{
    char out[M3U_PATH];
    int r = m3u_resolve(line, PL, out, sizeof(out));
    if (!want) {
        if (r == 0) printf("expected '%s' to be skipped, got '%s'\n", line, out);
        assert(r != 0);
        return;
    }
    if (r != 0 || strcmp(out, want)) printf("'%s' -> '%s' (%d), want '%s'\n", line, r ? "" : out, r, want);
    assert(r == 0 && !strcmp(out, want));
}

static Entry song(const char *path, const char *title, const char *artist, const char *album, int track)
{
    Entry e;
    memset(&e, 0, sizeof(e));
    snprintf(e.path, sizeof(e.path), "%s", path);
    snprintf(e.title, sizeof(e.title), "%s", title);
    snprintf(e.artist, sizeof(e.artist), "%s", artist);
    snprintf(e.album, sizeof(e.album), "%s", album);
    e.track = track;
    e.kind = KIND_MUSIC;
    return e;
}

static int cmp_mode;
static int cmp(const void *a, const void *b) { return msort_cmp(a, b, cmp_mode); }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* --- paths */
    expect_resolve("song.mp3", "ms0:/MUSIC/PLAYLISTS/song.mp3");
    expect_resolve("../Artist/Album/01 Song.mp3", "ms0:/MUSIC/Artist/Album/01 Song.mp3");
    expect_resolve("..\\Artist\\b.mp3", "ms0:/MUSIC/Artist/b.mp3");
    expect_resolve("./a/./b/../c.mp3", "ms0:/MUSIC/PLAYLISTS/a/c.mp3");
    expect_resolve("../../../../x.mp3", "ms0:/x.mp3"); /* not above the root */
    expect_resolve("/MUSIC/a.mp3", "ms0:/MUSIC/a.mp3");
    expect_resolve("MS0:/MUSIC/A.mp3", "ms0:/MUSIC/A.mp3");
    expect_resolve("ef0:/MUSIC/a.mp3", "ef0:/MUSIC/a.mp3");
    expect_resolve("ms0://MUSIC//a.mp3", NULL); /* reads as an address: skipped */
    expect_resolve("ms0:/MUSIC//a.mp3", "ms0:/MUSIC/a.mp3");
    expect_resolve("C:\\Users\\me\\Music\\Artist\\a.mp3", "ms0:/MUSIC/Artist/a.mp3");
    expect_resolve("D:\\Rips\\a.mp3", "ms0:/MUSIC/PLAYLISTS/a.mp3");
    expect_resolve("file:///C:/Users/me/Music/My%20Band/a.mp3", "ms0:/MUSIC/My Band/a.mp3");
    expect_resolve("file:///ms0:/MUSIC/a%2Bb.mp3", "ms0:/MUSIC/a+b.mp3");
    expect_resolve("  spaced.mp3  ", "ms0:/MUSIC/PLAYLISTS/spaced.mp3");
    expect_resolve("http://radio.example/stream", NULL);
    expect_resolve("https://x/y.mp3", NULL);
    expect_resolve("", NULL);
    expect_resolve("#EXTINF:1,x", NULL);
    expect_resolve("folder/", NULL);
    {
        char out[M3U_PATH];
        assert(m3u_resolve("song.mp3", "ef0:/MUSIC/old.m3u", out, sizeof(out)) == 0 && !strcmp(out, "ef0:/MUSIC/song.mp3"));
        char longname[400];
        memset(longname, 'a', sizeof(longname) - 1);
        longname[sizeof(longname) - 1] = 0;
        assert(m3u_resolve(longname, PL, out, sizeof(out)) != 0); /* too long for a library path */
    }

    /* --- a whole file: BOM, CRLF, comments, #EXTINF, Latin-1 and UTF-8 lines */
    const char text[] =
        "\xEF\xBB\xBF#EXTM3U\r\n"
        "#EXTINF:215,Band - First \r\n"
        "../Band/01 First.mp3\r\n"
        "\r\n"
        "# a comment\n"
        "../Caf\xE9/Ol\xE9.mp3\n"                       /* Latin-1 é */
        "#EXTINF:-1,\xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1\n" /* UTF-8 Greek */
        "/MUSIC/\xCE\x95\xCE\xBB\xCE\xBB\xCE\xAC\xCE\xB4\xCE\xB1.mp3\n"
        "http://stream.example/live\n"
        "#EXTINF:3,Only for the next line\n"
        "\n"
        "last.mp3"; /* no newline at the end */
    Got g = {0};
    int n = m3u_parse(text, (int)strlen(text), PL, got, &g);
    assert(n == 4 && g.n == 4);
    assert(!strcmp(g.path[0], "ms0:/MUSIC/Band/01 First.mp3") && !strcmp(g.title[0], "Band - First"));
    assert(!strcmp(g.path[1], "ms0:/MUSIC/Caf\xC3\xA9/Ol\xC3\xA9.mp3") && !g.title[1][0]);
    assert(!strcmp(g.path[2], "ms0:/MUSIC/\xCE\x95\xCE\xBB\xCE\xBB\xCE\xAC\xCE\xB4\xCE\xB1.mp3"));
    assert(!strcmp(g.title[2], "\xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1"));
    assert(!strcmp(g.path[3], "ms0:/MUSIC/PLAYLISTS/last.mp3")); /* the blank line in between kept the title? */
    assert(!strcmp(g.title[3], "Only for the next line"));
    assert(m3u_parse("", 0, PL, got, &g) == 0);
    assert(m3u_parse("#EXTM3U\n", 8, PL, NULL, NULL) == 0);

    /* --- relative paths for writing */
    char rel[M3U_PATH];
    m3u_relative("ms0:/MUSIC/PLAYLISTS", "ms0:/MUSIC/A/b.mp3", rel, sizeof(rel));
    assert(!strcmp(rel, "../A/b.mp3"));
    m3u_relative("ms0:/MUSIC/PLAYLISTS", "ms0:/music/playlists/c.mp3", rel, sizeof(rel)); /* FAT ignores case */
    assert(!strcmp(rel, "c.mp3"));
    m3u_relative("ms0:/MUSIC", "ms0:/MUSIC/A/B/c.mp3", rel, sizeof(rel));
    assert(!strcmp(rel, "A/B/c.mp3"));
    m3u_relative("ms0:/MUSIC/PLAYLISTS", "ms0:/PSP/MUSIC/x.mp3", rel, sizeof(rel));
    assert(!strcmp(rel, "../../PSP/MUSIC/x.mp3"));
    m3u_relative("ms0:/MUSIC/PLAYLISTS", "ef0:/MUSIC/x.mp3", rel, sizeof(rel));
    assert(!strcmp(rel, "ef0:/MUSIC/x.mp3"));

    /* --- writing, then reading back */
    const char *paths[] = {"ms0:/MUSIC/Band/01 First.mp3", "ms0:/MUSIC/PLAYLISTS/x.mp3", "ef0:/MUSIC/\xCE\x91.mp3"};
    const char *titles[] = {"First", NULL, "Two\nlines"};
    char buf[1024];
    int len = m3u_write(buf, sizeof(buf), PL, paths, titles, 3);
    assert(len > 0 && len == (int)strlen(buf));
    assert(!strcmp(buf, "#EXTM3U\n#EXTINF:-1,First\n../Band/01 First.mp3\nx.mp3\n#EXTINF:-1,Two lines\nef0:/MUSIC/\xCE\x91.mp3\n"));
    memset(&g, 0, sizeof(g));
    assert(m3u_parse(buf, len, PL, got, &g) == 3);
    for (int i = 0; i < 3; i++)
        assert(!strcmp(g.path[i], paths[i]));
    assert(!strcmp(g.title[2], "Two lines"));
    assert(m3u_write(buf, 20, PL, paths, titles, 3) == -1); /* too small */
    assert(m3u_write(buf, sizeof(buf), PL, paths, NULL, 0) == 8 && !strcmp(buf, "#EXTM3U\n"));

    assert(m3u_is_playlist("a.m3u") && m3u_is_playlist("B.M3U8") && !m3u_is_playlist("a.mp3") && !m3u_is_playlist(".m3u"));

    /* --- sort order */
    assert(natural_cmp("2 Song", "10 Song") < 0);
    assert(natural_cmp("track 007", "Track 7") == 0);
    assert(natural_cmp("abc", "ABD") < 0);
    assert(natural_cmp("ab", "abc") < 0);
    assert(natural_cmp("x10", "x9") > 0);
    assert(msort_parse("album") == MSORT_ALBUM && msort_parse("name") == MSORT_NAME && msort_parse("??") == MSORT_ARTIST);
    assert(!strcmp(msort_label(MSORT_NAME), "File name") && !strcmp(msort_key(MSORT_ALBUM), "album"));

    Entry s[6];
    s[0] = song("ms0:/MUSIC/z/10 b.mp3", "Beta", "Band", "Zeta", 2);
    s[1] = song("ms0:/MUSIC/a/2 a.mp3", "Alpha", "Band", "Zeta", 1);
    s[2] = song("ms0:/MUSIC/notag.mp3", "notag", "", "", 0);
    s[3] = song("ms0:/MUSIC/c.mp3", "Gamma", "Act", "Alpha", 0);
    s[4] = song("ms0:/MUSIC/1 d.mp3", "Delta", "Act", "", 0);
    s[5] = song("ms0:/MUSIC/d/9 e.mp3", "Eps", "", "Alpha", 3);
    Entry t[6];

    cmp_mode = MSORT_ARTIST; /* Act (no album first: ""), Act/Alpha, Band/Zeta 1, 2, then untagged by album, title */
    memcpy(t, s, sizeof(s));
    qsort(t, 6, sizeof(Entry), cmp);
    const char *want_artist[] = {"Delta", "Gamma", "Alpha", "Beta", "notag", "Eps"};
    for (int i = 0; i < 6; i++) {
        if (strcmp(t[i].title, want_artist[i])) printf("artist %d: %s want %s\n", i, t[i].title, want_artist[i]);
        assert(!strcmp(t[i].title, want_artist[i]));
    }
    cmp_mode = MSORT_ALBUM; /* Alpha: "" artist before Act? no: by artist name, "" first; then Zeta 1, 2; then no album */
    memcpy(t, s, sizeof(s));
    qsort(t, 6, sizeof(Entry), cmp);
    const char *want_album[] = {"Eps", "Gamma", "Alpha", "Beta", "notag", "Delta"};
    for (int i = 0; i < 6; i++) {
        if (strcmp(t[i].title, want_album[i])) printf("album %d: %s want %s\n", i, t[i].title, want_album[i]);
        assert(!strcmp(t[i].title, want_album[i]));
    }
    cmp_mode = MSORT_NAME; /* 1 d, 2 a, 9 e, 10 b, c, notag */
    memcpy(t, s, sizeof(s));
    qsort(t, 6, sizeof(Entry), cmp);
    const char *want_name[] = {"Delta", "Alpha", "Eps", "Beta", "Gamma", "notag"};
    for (int i = 0; i < 6; i++) {
        if (strcmp(t[i].title, want_name[i])) printf("name %d: %s want %s\n", i, t[i].title, want_name[i]);
        assert(!strcmp(t[i].title, want_name[i]));
    }
    /* the same name in two folders: the path decides, so the order is stable */
    Entry a = song("ms0:/MUSIC/a/x.mp3", "x", "", "", 0), b = song("ms0:/MUSIC/b/x.mp3", "x", "", "", 0);
    assert(msort_cmp(&a, &b, MSORT_NAME) < 0 && msort_cmp(&b, &a, MSORT_NAME) > 0);

    /* --- Games / Homebrew orders */
    {
        Entry g1 = song("ms0:/ISO/b.iso", "Beta 2", "", "", 0), g2 = song("ms0:/ISO/a.iso", "Beta 10", "", "", 0),
              g3 = song("ms0:/ISO/c.iso", "Alpha", "", "", 0);
        g1.size = 100; g2.size = 300; g3.size = 300;
        g1.mtime = 5; g2.mtime = 9; g3.mtime = 1;
        assert(gsort_cmp(&g1, &g2, GSORT_NAME, 0, 0) < 0);      /* Beta 2 before Beta 10 */
        assert(gsort_cmp(&g3, &g1, GSORT_NAME, 0, 0) < 0);
        assert(gsort_cmp(&g2, &g1, GSORT_SIZE, 0, 0) < 0);      /* bigger first */
        assert(gsort_cmp(&g3, &g2, GSORT_SIZE, 0, 0) < 0);      /* same size: by name */
        assert(gsort_cmp(&g2, &g1, GSORT_NEWEST, 0, 0) < 0);
        assert(gsort_cmp(&g1, &g3, GSORT_PLAYED, 50, 40) < 0);  /* played last first */
        assert(gsort_cmp(&g3, &g1, GSORT_PLAYED, 0, 40) > 0);   /* never played after */
        assert(gsort_cmp(&g3, &g1, GSORT_PLAYED, 0, 0) < 0);    /* both never: by name */
        assert(gsort_parse("played") == GSORT_PLAYED && gsort_parse("x") == GSORT_NAME);
        assert(!strcmp(gsort_label(GSORT_NEWEST), "Newest") && !strcmp(gsort_key(GSORT_SIZE), "size"));
    }

    puts("test_m3u: ok");
    return 0;
}
