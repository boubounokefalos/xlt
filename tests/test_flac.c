/* The FLAC decoder against files from the flac encoder (tests/make_audio.py): what
 * comes out has to equal the samples that went in, in every encoding, and after seeks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flac.h"

static int fails;
static const char *s_dir;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int file_read(void *ctx, uint64_t off, void *dst, uint32_t len)
{
    FILE *fp = ctx;
    if (fseek(fp, (long)off, SEEK_SET))
        return -1;
    return (int)fread(dst, 1, len, fp);
}

static FILE *open_flac(const char *name, Flac *f, int *r)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s.flac", s_dir, name);
    FILE *fp = fopen(p, "rb");
    if (!fp) { printf("missing %s\n", p); exit(1); }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    *r = flac_open(f, file_read, fp, (uint64_t)size);
    return fp;
}

static int32_t *load_pcm(const char *name, long *n)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s.pcm", s_dir, name);
    FILE *fp = fopen(p, "rb");
    if (!fp) { printf("missing %s\n", p); exit(1); }
    fseek(fp, 0, SEEK_END);
    long bytes = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    int32_t *b = malloc(bytes);
    if (fread(b, 1, bytes, fp) != (size_t)bytes) exit(1);
    fclose(fp);
    *n = bytes / 4;
    return b;
}

/* Decodes the whole file and compares it with the source samples. */
static void exact(const char *name, int rate, int channels, int bps)
{
    Flac f;
    int r;
    FILE *fp = open_flac(name, &f, &r);
    CHECK(r == 0, "%s opens: %d", name, r);
    if (r) { fclose(fp); return; }
    CHECK(f.rate == rate && f.channels == channels && f.bps == bps, "%s: %d Hz, %d ch, %d bit", name, f.rate, f.channels, f.bps);
    long n;
    int32_t *pcm = load_pcm(name, &n);
    long at = 0, wrong = 0;
    int b;
    while ((b = flac_decode(&f)) > 0) {
        for (int i = 0; i < b; i++)
            for (int c = 0; c < channels; c++, at++)
                if (at < n && pcm[at] != f.s[c][i]) wrong++;
    }
    CHECK(b == 0, "%s ends cleanly: %d", name, b);
    CHECK(at == n, "%s: %ld samples, wanted %ld", name, at, n);
    CHECK(wrong == 0, "%s: %ld samples differ", name, wrong);
    free(pcm);
    flac_close(&f);
    fclose(fp);
}

/* Seeks around and checks the first sample that comes out each time. */
static void seeks(const char *name)
{
    Flac f;
    int r;
    FILE *fp = open_flac(name, &f, &r);
    long n;
    int32_t *pcm = load_pcm(name, &n);
    long frames = n / f.channels;
    long targets[] = {0, 1, 4095, 4096, 4097, frames / 3, frames / 2, frames / 2 + 7, frames * 3 / 4, frames - 1, 12345};
    for (unsigned t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
        long at = targets[t];
        CHECK(flac_seek(&f, (uint64_t)at) == 0, "%s seek %ld", name, at);
        int b = flac_decode(&f);
        CHECK(b > 0, "%s decodes after seek to %ld: %d", name, at, b);
        if (b <= 0) continue;
        CHECK(f.frame_sample + f.skip == (uint64_t)at, "%s: lands at %llu+%u, wanted %ld", name,
              (unsigned long long)f.frame_sample, f.skip, at);
        long src = (long)f.frame_sample * f.channels;
        long wrong = 0;
        for (int i = 0; i < b; i++)
            for (int c = 0; c < f.channels; c++, src++)
                if (src < n && pcm[src] != f.s[c][i]) wrong++;
        CHECK(wrong == 0, "%s after seek to %ld: %ld wrong", name, at, wrong);
        f.skip = 0;
    }
    free(pcm);
    flac_close(&f);
    fclose(fp);
}

/* What the PSP is given. */
static void output(void)
{
    Flac f;
    int r;
    static int16_t out[70000 * 2];

    FILE *fp = open_flac("stereo", &f, &r);
    int b = flac_decode(&f);
    int n = flac_output(&f, out);
    CHECK(n == b, "16-bit stereo keeps every sample");
    CHECK(out[10] == f.s[0][5] && out[11] == f.s[1][5], "interleaved");
    flac_close(&f);
    fclose(fp);

    fp = open_flac("mono", &f, &r);
    flac_decode(&f);
    n = flac_output(&f, out);
    CHECK(out[6] == out[7] && out[6] == f.s[0][3], "mono on both sides");
    flac_close(&f);
    fclose(fp);

    fp = open_flac("bits24", &f, &r);
    flac_decode(&f);
    n = flac_output(&f, out);
    CHECK(out[4] == (int16_t)(f.s[0][2] >> 8), "24 bits to 16");
    flac_close(&f);
    fclose(fp);

    fp = open_flac("bits8", &f, &r);
    flac_decode(&f);
    n = flac_output(&f, out);
    CHECK(out[4] == (int16_t)(f.s[0][2] * 256), "8 bits to 16");
    flac_close(&f);
    fclose(fp);

    fp = open_flac("rate96", &f, &r);
    CHECK(r == 0 && f.out_rate == 48000 && f.step == 2, "96 kHz goes out at 48: %d, step %d", f.out_rate, f.step);
    b = flac_decode(&f);
    n = flac_output(&f, out);
    CHECK(n == (b + 1) / 2, "half the frames: %d of %d", n, b);
    CHECK(out[2] == (int16_t)(f.s[0][2] >> 8), "every second one");
    flac_close(&f);
    fclose(fp);

    fp = open_flac("ch6", &f, &r);
    CHECK(r == 0 && f.channels == 6, "six channels: %d", f.channels);
    b = flac_decode(&f);
    CHECK(b > 0, "they decode");
    n = flac_output(&f, out);
    CHECK(out[2] == f.s[0][1] && out[3] == f.s[1][1], "the front two");
    flac_close(&f);
    fclose(fp);

    fp = open_flac("stereo", &f, &r); /* a seek leaves out the start of the block */
    flac_seek(&f, 5000);
    b = flac_decode(&f);
    uint32_t skip = f.skip;
    n = flac_output(&f, out);
    CHECK(skip > 0 && n == b - (int)skip, "skip leaves out %u of %d: %d", skip, b, n);
    CHECK(f.skip == 0, "only once");
    flac_close(&f);
    fclose(fp);
}

static void damaged(void)
{
    Flac f;
    int r, b;
    long frames = 0;
    FILE *fp = open_flac("damaged", &f, &r);
    CHECK(r == 0, "damaged opens");
    while ((b = flac_decode(&f)) > 0) frames += b;
    CHECK(b == 0, "damage: carries on to the end: %d", b);
    CHECK(frames > 80000 && frames <= 100000, "damage costs a few frames: %ld of 100000", frames);
    flac_close(&f);
    fclose(fp);

    frames = 0;
    fp = open_flac("cut", &f, &r);
    CHECK(r == 0, "a cut file opens");
    while ((b = flac_decode(&f)) > 0) frames += b;
    CHECK(b == 0 && frames > 50000 && frames < 90000, "stops where it was cut: %d, %ld", b, frames);
    flac_close(&f);
    fclose(fp);

    fp = open_flac("notflac", &f, &r);
    CHECK(r == FLAC_ERR_FORMAT, "not a FLAC: %d", r);
    fclose(fp);
    fp = open_flac("odd_rate", &f, &r);
    CHECK(r == FLAC_ERR_RATE, "37.8 kHz can't be played: %d", r);
    fclose(fp);

    /* random damage anywhere must never crash (the sanitizers are watching) */
    char p[512];
    snprintf(p, sizeof(p), "%s/stereo.flac", s_dir);
    FILE *in = fopen(p, "rb");
    static unsigned char orig[2000000];
    int size = (int)fread(orig, 1, sizeof(orig), in);
    fclose(in);
    snprintf(p, sizeof(p), "%s/fuzz.flac", s_dir);
    unsigned seed = 99;
    for (int round = 0; round < 60; round++) {
        static unsigned char b2[2000000];
        memcpy(b2, orig, size);
        for (int i = 0; i < 300; i++) {
            seed = seed * 1103515245 + 12345;
            int at = 42 + (int)((seed >> 8) % (unsigned)(size - 42));
            seed = seed * 1103515245 + 12345;
            b2[at] = (unsigned char)(seed >> 16);
        }
        FILE *o = fopen(p, "wb");
        fwrite(b2, 1, size, o);
        fclose(o);
        fp = open_flac("fuzz", &f, &r);
        if (r == 0) {
            int guard = 0;
            while (flac_decode(&f) > 0 && guard++ < 200000) { static int16_t o2[70000 * 2]; flac_output(&f, o2); }
            flac_close(&f);
        }
        fclose(fp);
    }
}

static void tags(void)
{
    char p[512];
    Id3Info t;
    snprintf(p, sizeof(p), "%s/tagged.flac", s_dir);
    CHECK(flac_tags(p, &t) == 0, "tags read");
    CHECK(!strcmp(t.title, "A FLAC \xCE\xB1\xCE\xB2\xCE\xB3"), "title '%s'", t.title);
    CHECK(!strcmp(t.artist, "Some Band"), "artist '%s'", t.artist);
    CHECK(!strcmp(t.album, "Lossless"), "album '%s'", t.album);
    CHECK(t.track == 7, "track %d", t.track);
    CHECK(t.art_size > 0, "cover found");
    FILE *fp = fopen(p, "rb");
    unsigned char m[4] = {0};
    fseek(fp, (long)t.art_off, SEEK_SET);
    CHECK(fread(m, 1, 4, fp) == 4 && !memcmp(m, "\x89PNG", 4), "cover is where it says");
    fclose(fp);
    char lyr[256];
    CHECK(flac_lyrics(p, lyr, sizeof(lyr)) == 0 && !strncmp(lyr, "[00:01.00]line one", 18), "lyrics '%s'", lyr);
    char small[8];
    CHECK(flac_lyrics(p, small, sizeof(small)) == 0 && strlen(small) == 7, "lyrics cut to fit");

    snprintf(p, sizeof(p), "%s/untagged.flac", s_dir);
    CHECK(flac_tags(p, &t) == 0 && !t.title[0] && !t.art_size, "no tags");
    CHECK(flac_lyrics(p, lyr, sizeof(lyr)) != 0, "no lyrics");
    snprintf(p, sizeof(p), "%s/with_id3.flac", s_dir);
    CHECK(flac_tags(p, &t) == 0, "an ID3 tag in front");
    snprintf(p, sizeof(p), "%s/notflac.flac", s_dir);
    CHECK(flac_tags(p, &t) != 0, "not a FLAC");
    snprintf(p, sizeof(p), "%s/nothere.flac", s_dir);
    CHECK(flac_tags(p, &t) != 0, "missing file");
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    s_dir = argv[1];
    exact("stereo", 44100, 2, 16);
    exact("best", 44100, 2, 16);
    exact("fast", 44100, 2, 16);
    exact("mono", 44100, 1, 16);
    exact("bits24", 44100, 2, 24);
    exact("bits8", 44100, 2, 8);
    exact("small_block", 44100, 2, 16);
    exact("large_block", 44100, 2, 16);
    exact("no_seektable", 44100, 2, 16);
    exact("rate96", 96000, 2, 24);
    exact("rate22", 22050, 2, 16);
    exact("ch6", 44100, 6, 16);
    exact("with_id3", 44100, 2, 16);
    seeks("stereo");
    seeks("no_seektable");
    seeks("large_block");
    seeks("small_block");
    output();
    damaged();
    tags();
    if (fails) {
        printf("test_flac: %d failed\n", fails);
        return 1;
    }
    puts("test_flac: ok");
    return 0;
}
