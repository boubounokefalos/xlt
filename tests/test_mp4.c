/* Host test: the MP4 demuxer against ffprobe's packet list (every sample's
 * offset, size, key flag and decode time), plus stream parameters. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/mp4.h"
#include "../src/srt.h"

static int fails;
#define CHECK(cond, ...) do { int c_ = (cond); char m_[256]; snprintf(m_, sizeof(m_), __VA_ARGS__); \
    if (c_) printf("OK   %s\n", m_); else { printf("FAIL %s\n", m_); fails++; } } while (0)

/* mp4_sub_read's results, in the order they came */
static int n_got, got_k[8], got_len[8];
static char *got_txt[8];
static void got_track(int k, char *text, int len, void *ctx)
{
    (void)ctx;
    if (n_got < 8) { got_k[n_got] = k; got_txt[n_got] = text; got_len[n_got] = len; n_got++; }
    else free(text);
}

/* compares track t with ffprobe's csv; returns the number of packets */
static int compare(const char *csv, const Mp4Track *t, const char *what)
{
    FILE *f = fopen(csv, "r");
    if (!f) { CHECK(0, "%s: no reference", what); return 0; }
    double dts0 = 0;
    int n = 0, bad_off = 0, bad_size = 0, bad_key = 0, bad_time = 0;
    double dts;
    unsigned size;
    unsigned long long pos;
    char flags[16];
    while (fscanf(f, "%lf,%u,%llu,%15s", &dts, &size, &pos, flags) == 4) {
        if (n == 0) dts0 = dts;
        if ((uint32_t)n < t->count) {
            if (mp4_offset(t, n) != pos) bad_off++;
            if (mp4_size(t, n) != size) bad_size++;
            if (mp4_is_sync(t, n) != (flags[0] == 'K')) bad_key++;
            long want = (long)((dts - dts0) * 1000 + 0.5), got = mp4_time_ms(t, n);
            if (labs(want - got) > 1) bad_time++;
        }
        n++;
    }
    fclose(f);
    CHECK((uint32_t)n == t->count, "%s: %u samples (ffprobe %d)", what, t->count, n);
    /* chunks: every sample lies inside its chunk's contiguous byte run */
    int bad_chunk = 0;
    for (uint32_t i = 0; i < t->count; i++) {
        uint32_t first, count;
        mp4_chunk_of(t, i, &first, &count);
        uint64_t off = mp4_offset(t, first);
        for (uint32_t s = first; s < i; s++) off += mp4_size(t, s);
        if (i < first || i >= first + count || off != mp4_offset(t, i)) bad_chunk++;
    }
    CHECK(!bad_chunk, "%s: chunk ranges cover each sample contiguously (%d wrong)", what, bad_chunk);
    CHECK(!bad_off && !bad_size, "%s: every offset and size matches (%d / %d wrong)", what, bad_off, bad_size);
    CHECK(!bad_key, "%s: key frames match stss (%d wrong)", what, bad_key);
    CHECK(!bad_time, "%s: decode times within 1 ms (%d off)", what, bad_time);
    return n;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *d = argc > 1 ? argv[1] : "build/video";
    char p[256], c[256];
    Mp4 m;
    const char *why = "";

    snprintf(p, sizeof(p), "%s/psp_faststart.mp4", d);
    CHECK(mp4_open(&m, p, &why) == 0, "baseline 480x272 + AAC opens (%s)", why);
    CHECK(m.width == 480 && m.height == 272 && m.profile == 66, "480x272, Baseline (profile %d)", m.profile);
    CHECK(m.nal_length == 4 && m.sps_size > 4 && (m.sps[0] & 0x1F) == 7 && (m.pps[0] & 0x1F) == 8,
          "avcC: 4-byte NAL lengths, SPS/PPS NAL types 7/8");
    CHECK(m.audio.present && m.sample_rate == 44100 && m.channels == 2 && m.aac_object == 2,
          "AAC LC 44.1 kHz stereo (%d Hz, %d ch, obj %d)", m.sample_rate, m.channels, m.aac_object);
    CHECK(m.duration_ms >= 2900 && m.duration_ms <= 3200, "duration %u ms", m.duration_ms);
    snprintf(c, sizeof(c), "%s/psp_faststart.v.csv", d); compare(c, &m.video, "faststart video");
    snprintf(c, sizeof(c), "%s/psp_faststart.a.csv", d); compare(c, &m.audio, "faststart audio");
    int wp = -1, wb = -1;
    CHECK(h264_pps_weights(m.pps, m.pps_size, &wp, &wb) == 0 && wp == 0 && wb == 0,
          "Baseline PPS: no weighted prediction (wp %d, wb %d)", wp, wb);
    CHECK(h264_sps_refs(m.sps, m.sps_size) == 3, "Baseline SPS (POC type 2): 3 reference frames (%d)",
          h264_sps_refs(m.sps, m.sps_size));
    {
        FILE *f = fopen(p, "rb");
        static uint8_t buf[1 << 16];
        int idr0 = 0, idr1 = 1;
        if (f) {
            uint32_t n0 = mp4_size(&m.video, 0), n1 = mp4_size(&m.video, 1);
            fseek(f, (long)mp4_offset(&m.video, 0), SEEK_SET);
            if (n0 <= sizeof(buf) && fread(buf, 1, n0, f) == n0) idr0 = h264_sample_has_idr(buf, n0, m.nal_length);
            fseek(f, (long)mp4_offset(&m.video, 1), SEEK_SET);
            if (n1 <= sizeof(buf) && fread(buf, 1, n1, f) == n1) idr1 = h264_sample_has_idr(buf, n1, m.nal_length);
            fclose(f);
        }
        CHECK(idr0 == 1 && idr1 == 0, "IDR detected in sample 0, not in sample 1 (%d, %d)", idr0, idr1);
    }
    uint32_t s = mp4_sample_at(&m.video, 1500);
    CHECK(mp4_time_ms(&m.video, s) <= 1500 && mp4_time_ms(&m.video, s + 1) > 1500, "sample_at(1500 ms) = %u", s);
    uint32_t k = mp4_sync_before(&m.video, s);
    CHECK(mp4_is_sync(&m.video, k) && k <= s && s - k < 15, "key frame before it: %u", k);
    {
        int bad = 0;
        for (uint32_t n = 0; n < m.video.count; n++) {
            uint32_t a = mp4_sync_after(&m.video, n);
            if (a < n || (a < m.video.count && !mp4_is_sync(&m.video, a))) bad++;
            for (uint32_t b = n; b < a && b < m.video.count; b++)
                if (mp4_is_sync(&m.video, b)) bad++;
        }
        CHECK(!bad && mp4_sync_after(&m.video, m.video.count) == m.video.count,
              "sync_after: always the first key frame at or after a sample");
    }
    char why2[96];
    CHECK(h264_psp_problem(&m, 0, why2, sizeof(why2)) == 0, "a PSP-style encode has nothing against it");
    mp4_close(&m);

    snprintf(p, sizeof(p), "%s/moov_at_end.mp4", d);
    CHECK(mp4_open(&m, p, &why) == 0 && m.profile == 77 && m.sample_rate == 48000, "moov at the end, Main + B-frames, 48 kHz");
    snprintf(c, sizeof(c), "%s/moov_at_end.v.csv", d); compare(c, &m.video, "B-frame video");
    wp = wb = -1;
    CHECK(h264_pps_weights(m.pps, m.pps_size, &wp, &wb) == 0 && wp == 1 && wb == 2,
          "x264 Main PPS: weighted P on, implicit B weights (wp %d, wb %d)", wp, wb);
    CHECK(h264_sps_refs(m.sps, m.sps_size) == 4, "Main SPS (POC type 0): 4 reference frames (%d)",
          h264_sps_refs(m.sps, m.sps_size));
    snprintf(c, sizeof(c), "%s/moov_at_end.a.csv", d); compare(c, &m.audio, "48 kHz audio");
    CHECK(h264_psp_problem(&m, 0, why2, sizeof(why2)) == 1 && strstr(why2, "weighted prediction"),
          "x264 defaults: weighted prediction named (%s)", why2);
    mp4_close(&m);
    { /* the other reasons, from the numbers alone (no SPS/PPS to read) */
        Mp4 f;
        memset(&f, 0, sizeof(f));
        f.profile = 100; f.level = 30; f.width = 480; f.height = 272;
        CHECK(h264_psp_problem(&f, 0, why2, sizeof(why2)) == 1 && strstr(why2, "High profile"), "High profile (%s)", why2);
        f.profile = 77; f.level = 31;
        CHECK(h264_psp_problem(&f, 0, why2, sizeof(why2)) == 1 && strstr(why2, "level 3.1"), "level 3.1 (%s)", why2);
        f.level = 30; f.width = 1280; f.height = 720;
        CHECK(h264_psp_problem(&f, 0, why2, sizeof(why2)) == 1 && strstr(why2, "1280x720"), "720p (%s)", why2);
        f.width = 480; f.height = 272;
        CHECK(h264_psp_problem(&f, 0, why2, sizeof(why2)) == 0, "Main 3.0 480x272: nothing known against it");
        CHECK(strstr(H264_PSP_FIX, "weightp=0") != NULL, "the fix names the x264 setting");
    }

    snprintf(p, sizeof(p), "%s/video_only.mp4", d);
    CHECK(mp4_open(&m, p, &why) == 0 && !m.audio.present && m.width == 320, "video only (no audio track)");
    snprintf(c, sizeof(c), "%s/video_only.v.csv", d); compare(c, &m.video, "video-only");
    mp4_close(&m);

    snprintf(p, sizeof(p), "%s/two_audio.mp4", d);
    CHECK(mp4_open(&m, p, &why) == 0 && m.naudio == 2, "two audio tracks listed (%d)", m.naudio);
    CHECK(!strcmp(m.audios[0].lang, "eng") && !strcmp(m.audios[1].lang, "jpn"), "their languages (%s, %s)",
          m.audios[0].lang, m.audios[1].lang);
    CHECK(m.audios[0].name[0] == 0 && !strcmp(m.audios[1].name, "Director's commentary"),
          "a title kept, the muxer's \"SoundHandler\" not (\"%s\", \"%s\")", m.audios[0].name, m.audios[1].name);
    CHECK(m.audios[0].enabled && !m.audios[1].enabled, "the default is the enabled one");
    CHECK(m.audio_sel == 0 && m.sample_rate == 44100, "the default loaded (%d Hz)", m.sample_rate);
    mp4_close(&m);
    CHECK(mp4_open_track(&m, p, &why, 1, NULL) == 0 && m.audio_sel == 1 && m.sample_rate == 48000 && m.naudio == 2,
          "track 2 by index (%d Hz)", m.sample_rate);
    snprintf(c, sizeof(c), "%s/two_audio.a1.csv", d); compare(c, &m.audio, "second audio track");
    CHECK(m.video.count > 0 && m.width == 480, "the video with it");
    mp4_close(&m);
    CHECK(mp4_open_track(&m, p, &why, -1, "jpn") == 0 && m.audio_sel == 1, "by language: jpn");
    mp4_close(&m);
    CHECK(mp4_open_track(&m, p, &why, -1, "fre") == 0 && m.audio_sel == 0, "a language it hasn't: the default");
    mp4_close(&m);
    CHECK(mp4_open_track(&m, p, &why, 7, NULL) == 0 && m.audio_sel == 0, "an index out of range: the default");
    mp4_close(&m);
    CHECK(!strcmp(mp4_lang_name("jpn"), "Japanese") && !strcmp(mp4_lang_name("xyz"), "xyz"), "language names");

    snprintf(p, sizeof(p), "%s/subs.mp4", d);
    CHECK(mp4_open(&m, p, &why) == 0 && m.nsub == 2, "two subtitle tracks listed, the chapter track not (%d)", m.nsub);
    CHECK(!strcmp(m.subs[0].lang, "eng") && !strcmp(m.subs[1].lang, "gre"), "their languages (%s, %s)",
          m.subs[0].lang, m.subs[1].lang);
    CHECK(m.subs[0].name[0] == 0 && !strcmp(m.subs[1].name, "Greek forced"),
          "a title kept, \"SubtitleHandler\" not (\"%s\", \"%s\")", m.subs[0].name, m.subs[1].name);
    CHECK(m.subs[0].enabled && !m.subs[1].enabled, "the default subtitle track is the enabled one");
    mp4_close(&m);
    {
        int order[2] = {1, 0}; /* the Greek one first, as the player asks for the one it shows */
        n_got = 0;
        int n = mp4_sub_read(p, order, 2, got_track, NULL, NULL);
        CHECK(n == 2 && n_got == 2 && got_k[0] == 1 && got_k[1] == 0 && got_txt[0] && got_txt[1],
              "both tracks read, in the order asked (%d: %d, %d)", n, got_k[0], got_k[1]);
        if (n_got == 2 && got_txt[0] && got_txt[1]) {
            Srt s0, s1; /* s0 English, s1 Greek */
            int c0 = srt_parse(&s0, got_txt[1], got_len[1]), c1 = srt_parse(&s1, got_txt[0], got_len[0]);
            CHECK(c0 == 2 && s0.cue[0].start == 200 && s0.cue[0].end == 900 && !strcmp(s0.cue[0].text, "Hello there"),
                  "English cue 1 (%d cues, %d-%d \"%s\")", c0, c0 ? s0.cue[0].start : -1, c0 ? s0.cue[0].end : -1,
                  c0 ? s0.cue[0].text : "");
            CHECK(c0 == 2 && s0.cue[1].start == 1000 && s0.cue[1].end == 1800 && !strcmp(s0.cue[1].text, "Two lines\nsecond"),
                  "English cue 2, two lines (\"%s\")", c0 == 2 ? s0.cue[1].text : "");
            CHECK(c1 == 1 && s1.cue[0].start == 300 && s1.cue[0].end == 1500 &&
                  !strcmp(s1.cue[0].text, "\316\232\316\261\316\273\316\267\316\274\341\275\263\317\201\316\261"),
                  "Greek cue, UTF-8 kept (\"%s\")", c1 ? s1.cue[0].text : "");
            CHECK(srt_at(&s0, 950) == NULL && srt_at(&s0, 1500) == &s0.cue[1], "the gap between cues shows nothing");
            srt_free(&s0);
            srt_free(&s1);
        }
        for (int k = 0; k < n_got; k++) free(got_txt[k]);
        int one = 0;
        n_got = 0;
        CHECK(mp4_sub_read(p, &one, 1, got_track, NULL, NULL) == 1 && n_got == 1 && got_k[0] == 0, "one track alone");
        for (int k = 0; k < n_got; k++) free(got_txt[k]);
        volatile int stop = 1;
        n_got = 0;
        CHECK(mp4_sub_read(p, order, 2, got_track, NULL, &stop) == 0 && n_got == 0, "stopped: nothing handed over");
    }
    snprintf(p, sizeof(p), "%s/two_audio.mp4", d);
    {
        int order[1] = {0};
        n_got = 0;
        CHECK(mp4_sub_read(p, order, 1, got_track, NULL, NULL) == 0 && n_got == 0, "no subtitle tracks: none read");
    }

    snprintf(p, sizeof(p), "%s/fragmented.mp4", d);
    CHECK(mp4_open(&m, p, &why) != 0, "fragmented MP4 refused (%s)", why);
    snprintf(p, sizeof(p), "%s/truncated.mp4", d);
    CHECK(mp4_open(&m, p, &why) != 0, "file cut inside its index refused (%s)", why);
    CHECK(mp4_open(&m, "/nonexistent.mp4", &why) != 0, "missing file refused (%s)", why);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
