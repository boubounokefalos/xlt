/* M4A and ADTS reading against files from ffmpeg (tests/make_audio.py). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audiofmt.h"
#include "mp4.h"

static int fails;
static const char *s_dir;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void path_of(char *p, int n, const char *name) { snprintf(p, n, "%s/%s", s_dir, name); }

static long load(const char *name, unsigned char *b, long cap)
{
    char p[512];
    path_of(p, sizeof(p), name);
    FILE *f = fopen(p, "rb");
    if (!f) { printf("missing %s\n", p); exit(1); }
    long n = (long)fread(b, 1, cap, f);
    fclose(f);
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    s_dir = argv[1];
    char p[512];

    CHECK(audio_format("a.mp3") == AUDIO_MP3, "mp3");
    CHECK(audio_format("ms0:/MUSIC/A.MP3") == AUDIO_MP3, "case");
    CHECK(audio_format("a.flac") == AUDIO_FLAC, "flac");
    CHECK(audio_format("a.FLAC") == AUDIO_FLAC, "FLAC");
    CHECK(audio_format("a.m4a") == AUDIO_M4A, "m4a");
    CHECK(audio_format("a.aac") == AUDIO_AAC, "aac");
    CHECK(audio_format("a.mp4") == AUDIO_NONE, "a video is not music");
    CHECK(audio_format("a.txt") == AUDIO_NONE, "text");
    CHECK(audio_format("flac") == AUDIO_NONE, "no extension");
    CHECK(audio_format("x.flac/readme") == AUDIO_NONE, "a dot in a folder name");
    CHECK(audio_rate_ok(44100) && audio_rate_ok(48000) && audio_rate_ok(8000), "rates the PSP takes");
    CHECK(!audio_rate_ok(96000) && !audio_rate_ok(7350) && !audio_rate_ok(0), "rates it doesn't");

    /* M4A */
    Mp4 m;
    const char *why = "";
    path_of(p, sizeof(p), "plain.m4a");
    CHECK(mp4_open(&m, p, &why) != 0, "the video player still wants video");
    CHECK(mp4_open_audio(&m, p, &why) == 0, "audio only: %s", why);
    CHECK(m.audio.present && !m.video.present, "the audio track");
    CHECK(m.aac_object == 2 && m.sample_rate == 44100, "object %d, %d Hz", m.aac_object, m.sample_rate);
    CHECK(m.audio.count > 200, "%u frames", m.audio.count);
    CHECK(m.duration_ms > 5800 && m.duration_ms < 6200, "%u ms", m.duration_ms);
    uint32_t mid = mp4_sample_at(&m.audio, 3000);
    CHECK(mid > 100 && mid < m.audio.count - 50, "sample at 3 s: %u", mid);
    uint32_t at = mp4_time_ms(&m.audio, mid);
    CHECK(at > 2950 && at <= 3000, "time %u", at);
    mp4_close(&m);

    path_of(p, sizeof(p), "alac.m4a");
    CHECK(mp4_open_audio(&m, p, &why) != 0, "ALAC is refused");
    path_of(p, sizeof(p), "video.mp4");
    CHECK(mp4_open_audio(&m, p, &why) == 0, "a video's sound opens too: %s", why);
    mp4_close(&m);
    path_of(p, sizeof(p), "plain.aac");
    CHECK(mp4_open_audio(&m, p, &why) != 0, "ADTS isn't MP4");

    /* tags */
    Id3Info t;
    path_of(p, sizeof(p), "tagged.m4a");
    CHECK(audio_tags(p, 0, &t) == 0, "m4a tags");
    CHECK(!strcmp(t.title, "An M4A \xCE\xB1\xCE\xB2"), "title '%s'", t.title);
    CHECK(!strcmp(t.artist, "AAC Band"), "artist '%s'", t.artist);
    CHECK(!strcmp(t.album, "Compressed"), "album '%s'", t.album);
    CHECK(t.track == 3, "track %d", t.track);
    CHECK(t.art_size > 0, "cover found");
    static unsigned char b[300000];
    long n = load("tagged.m4a", b, sizeof(b));
    CHECK((long)t.art_off + 4 < n && !memcmp(b + t.art_off, "\x89PNG", 4), "cover is where it says");
    char lyr[128];
    CHECK(audio_lyrics(p, lyr, sizeof(lyr)) == 0 && !strcmp(lyr, "some words"), "lyrics '%s'", lyr);
    path_of(p, sizeof(p), "plain.m4a");
    CHECK(audio_tags(p, 0, &t) == 0 && !t.title[0] && !t.art_size, "no tags");
    CHECK(audio_lyrics(p, lyr, sizeof(lyr)) != 0, "no lyrics");
    path_of(p, sizeof(p), "tagged.flac");
    CHECK(audio_tags(p, 0, &t) == 0 && t.track == 7, "flac through the same door");
    CHECK(audio_lyrics(p, lyr, sizeof(lyr)) == 0, "flac lyrics");
    path_of(p, sizeof(p), "notflac.flac");
    CHECK(audio_tags(p, 1500, &t) != 0, "a false .flac");

    /* ADTS */
    n = load("plain.aac", b, sizeof(b));
    Adts a;
    CHECK(adts_parse(b, (int)n, &a) == 0, "first frame");
    CHECK(a.object == 2 && a.rate == 44100 && a.channels == 1 && a.header == 7, "object %d, %d Hz, %d ch, header %d", a.object, a.rate, a.channels, a.header);
    long off = 0;
    int frames = 0;
    while (off < n && adts_parse(b + off, (int)(n - off), &a) == 0) { off += a.frame; frames++; }
    CHECK(off == n, "the frames add up: %ld of %ld", off, n);
    CHECK(frames > 200, "%d frames", frames);
    CHECK(adts_find(b, (int)n) == 0, "starts at 0");
    long mid_off = 0; /* from the middle of a frame, the next whole one */
    for (int i = 0; i < 50; i++) { adts_parse(b + mid_off, 10, &a); mid_off += a.frame; }
    long inside = mid_off - 11;
    int f = adts_find(b + inside, 4096);
    CHECK(f == 11, "resync: %d", f);
    unsigned char junk[64];
    memset(junk, 0, sizeof(junk));
    CHECK(adts_find(junk, sizeof(junk)) == -1, "nothing in zeros");
    CHECK(adts_parse(junk, 6, &a) != 0, "too short");

    /* loudness tags, and what levelling does with them */
    path_of(p, sizeof(p), "gain.flac");
    CHECK(audio_tags(p, 0, &t) == 0 && t.gains == (GAIN_TRACK | GAIN_ALBUM) && t.gain_track == -650 && t.gain_album == -425,
          "FLAC ReplayGain: %x %d %d", t.gains, t.gain_track, t.gain_album);
    path_of(p, sizeof(p), "gain.m4a");
    CHECK(audio_tags(p, 0, &t) == 0 && (t.gains & GAIN_TRACK) && t.gain_track == -801, "M4A replaygain_track_gain: %x %d",
          t.gains, t.gain_track);
    CHECK((t.gains & GAIN_SC) && t.gain_sc == 97, "M4A iTunNORM 800 -> +0.97 dB: %d", t.gain_sc);
    CHECK(audio_gain(&t, LEVEL_SONG) == -801, "ReplayGain wins over Sound Check");
    path_of(p, sizeof(p), "soundcheck.m4a");
    CHECK(audio_tags(p, 0, &t) == 0 && t.gains == GAIN_SC && t.gain_sc == -903, "M4A Sound Check only: %x %d", t.gains,
          t.gain_sc);
    CHECK(audio_gain(&t, LEVEL_ALBUM) == -903, "album mode falls back on Sound Check");
    path_of(p, sizeof(p), "plain.m4a");
    CHECK(audio_tags(p, 0, &t) == 0 && t.gains == 0 && audio_gain(&t, LEVEL_SONG) == GAIN_NONE, "untagged: no gain");

    memset(&t, 0, sizeof(t));
    t.gains = GAIN_TRACK | GAIN_ALBUM;
    t.gain_track = -900;
    t.gain_album = -500;
    CHECK(audio_gain(&t, LEVEL_SONG) == -900 && audio_gain(&t, LEVEL_ALBUM) == -500, "song and album modes");
    CHECK(audio_gain(&t, LEVEL_OFF) == GAIN_NONE, "off");
    t.gains = GAIN_ALBUM;
    CHECK(audio_gain(&t, LEVEL_SONG) == -500, "song mode falls back on the album's");
    const int MAX = 0x8000;
    CHECK(audio_level_volume(GAIN_NONE, MAX) == MAX, "no gain: full volume");
    CHECK(audio_level_volume(+350, MAX) == MAX && audio_level_volume(-600, MAX) == MAX, "never louder: up to -6 dB untouched");
    int v = audio_level_volume(-1200, MAX); /* 6 dB cut: half the amplitude */
    CHECK(v > 16400 && v < 16450, "-12 dB -> 6 dB cut: %d (16423)", v);
    v = audio_level_volume(-850, MAX); /* 2.5 dB cut */
    CHECK(v > 24540 && v < 24590, "-8.5 dB -> 2.5 dB cut: %d (24565)", v);
    v = audio_level_volume(-9000, MAX);
    CHECK(v > 0 && v < 400, "a huge cut is held at 34 dB: %d", v);

    if (fails) {
        printf("test_audiofmt: %d failed\n", fails);
        return 1;
    }
    puts("test_audiofmt: ok");
    return 0;
}
