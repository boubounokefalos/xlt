/* Host test for ID3 reading. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "../src/id3.h"
#include "../src/imgdecode.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("OK   %s\n", msg); else { printf("FAIL %s\n", msg); fails++; } } while (0)

static int rd(const char *dir, const char *name, Id3Info *o, char path[256])
{
    struct stat st;
    snprintf(path, 256, "%s/%s", dir, name);
    stat(path, &st);
    return id3_read(path, st.st_size, o);
}

int main(int argc, char **argv)
{
    const char *d = argc > 1 ? argv[1] : "build/mp3";
    char p[256], msg[200];
    Id3Info o;

    CHECK(rd(d, "v23.mp3", &o, p) == 0, "v2.3 read");
    CHECK(!strcmp(o.title, "Midnight City"), "v2.3 UTF-16 (BOM) title");
    CHECK(!strcmp(o.artist, "M83") && !strcmp(o.album, "Hurry Up, We're Dreaming"), "v2.3 Latin-1 artist/album");
    CHECK(o.track == 3, "track '3/12' -> 3");
    unsigned want_off = 0, want_size = 0, tag_end = 0;
    char e[256];
    sprintf(e, "%s/v23.expect", d);
    FILE *f = fopen(e, "r");
    if (f) { if (fscanf(f, "%u %u %u", &want_off, &want_size, &tag_end) != 3) want_off = 0; fclose(f); }
    snprintf(msg, sizeof(msg), "front cover chosen over the earlier back cover (off %u size %u)", o.art_off, o.art_size);
    CHECK(o.art_off == want_off && o.art_size == want_size, msg);
    CHECK(o.audio_start == tag_end, "audio starts after the tag");
    {
        static unsigned char art[80 * 80 * 4];
        int w, h;
        int r = img_decode_at(p, o.art_off, IMG_COVER, 80, 80, art, &w, &h);
        /* the front cover is solid red (200,30,30) */
        CHECK(r == 0 && w == 80 && h == 80 && art[0] > 170 && art[1] < 60 && art[2] < 60,
              "embedded front cover decodes in place (img_decode_at)");
    }

    CHECK(rd(d, "v24.mp3", &o, p) == 0 && !strcmp(o.title, "Caf\xc3\xa9 Song"), "v2.4 UTF-8 title");
    CHECK(!strcmp(o.artist, "Artist BE") && o.track == 7, "v2.4 UTF-16BE artist, track");
    CHECK(o.art_size > 0, "v2.4 PNG picture with an empty description");

    CHECK(rd(d, "v22.mp3", &o, p) == 0 && !strcmp(o.title, "Old Tag") && !strcmp(o.artist, "Old Artist"), "v2.2 TT2/TP1");
    CHECK(o.art_size > 0, "v2.2 PIC picture");

    CHECK(rd(d, "v1.mp3", &o, p) == 0 && !strcmp(o.title, "Sp\xc3\xa4t") && !strcmp(o.artist, "Band"), "ID3v1 Latin-1 -> UTF-8, padding trimmed");
    CHECK(!strcmp(o.album, "Disc") && o.track == 9, "ID3v1 album and v1.1 track");
    CHECK(rd(d, "notag.mp3", &o, p) == 0 && !o.title[0] && !o.art_size && o.audio_start == 0, "no tag: empty, audio at 0");
    CHECK(rd(d, "bad.mp3", &o, p) == 0 && !o.title[0], "oversized frame inside a short tag is ignored");
    {
        char ly[512];
        snprintf(p, sizeof(p), "%s/lyrics24.mp3", d);
        CHECK(id3_lyrics(p, ly, sizeof(ly)) == 0 && !strcmp(ly, "[00:01.00]Hello\n[00:02.50]\xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1\n"),
              "v2.4 USLT: timed lyrics in UTF-8");
        snprintf(p, sizeof(p), "%s/lyrics23.mp3", d);
        CHECK(id3_lyrics(p, ly, sizeof(ly)) == 0 && !strcmp(ly, "Line one\nLine two"), "v2.3 USLT: UTF-16, after its description");
        snprintf(p, sizeof(p), "%s/v23.mp3", d);
        CHECK(id3_lyrics(p, ly, sizeof(ly)) != 0 && !ly[0], "no USLT: none");
    }
    /* loudness tags */
    CHECK(rd(d, "gain23.mp3", &o, p) == 0 && !strcmp(o.title, "Gain Song"), "gain23 read");
    CHECK((o.gains & GAIN_TRACK) && o.gain_track == -723, "TXXX REPLAYGAIN_TRACK_GAIN -7.23 dB (Latin-1)");
    CHECK((o.gains & GAIN_ALBUM) && o.gain_album == -510, "TXXX replaygain_album_gain -5.10 dB (UTF-16, lower case)");
    CHECK(!(o.gains & GAIN_SC), "an ordinary comment isn't Sound Check");
    CHECK(rd(d, "soundcheck24.mp3", &o, p) == 0 && o.gains == GAIN_SC, "COMM iTunNORM found (v2.4)");
    snprintf(msg, sizeof(msg), "Sound Check 8000/7000 -> -9.03 dB (got %d)", o.gain_sc);
    CHECK(o.gain_sc == -903, msg);
    CHECK(rd(d, "gain22.mp3", &o, p) == 0 && o.gains == GAIN_TRACK && o.gain_track == 150, "v2.2 TXX +1.5 dB");
    CHECK(rd(d, "v23.mp3", &o, p) == 0 && o.gains == 0, "no loudness tags: none");
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
