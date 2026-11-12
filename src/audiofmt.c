/* XLT - music file formats. See audiofmt.h. */
#include <string.h>
#include <strings.h>
#include "audiofmt.h"
#include "flac.h"
#include "mp4.h"

int audio_format(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return AUDIO_NONE;
    if (!strcasecmp(dot, ".mp3")) return AUDIO_MP3;
    if (!strcasecmp(dot, ".aac")) return AUDIO_AAC;
    if (!strcasecmp(dot, ".m4a")) return AUDIO_M4A;
    if (!strcasecmp(dot, ".flac")) return AUDIO_FLAC;
    return AUDIO_NONE;
}

const char *audio_format_name(int format)
{
    switch (format) {
    case AUDIO_MP3: return "MP3 audio";
    case AUDIO_AAC: return "AAC audio";
    case AUDIO_M4A: return "M4A audio";
    case AUDIO_FLAC: return "FLAC audio";
    default: return "Audio";
    }
}

int audio_tags(const char *path, uint64_t file_size, Id3Info *out)
{
    switch (audio_format(path)) {
    case AUDIO_M4A: return mp4_tags(path, out);
    case AUDIO_FLAC: return flac_tags(path, out);
    default: return id3_read(path, file_size, out); /* MP3, and .aac files that carry an ID3 tag */
    }
}

int audio_lyrics(const char *path, char *out, int cap)
{
    switch (audio_format(path)) {
    case AUDIO_M4A: return mp4_lyrics(path, out, cap);
    case AUDIO_FLAC: return flac_lyrics(path, out, cap);
    default: return id3_lyrics(path, out, cap);
    }
}

int audio_gain(const Id3Info *t, int mode)
{
    if (mode == LEVEL_OFF) return GAIN_NONE;
    if (mode == LEVEL_ALBUM && (t->gains & GAIN_ALBUM)) return t->gain_album;
    if (t->gains & GAIN_TRACK) return t->gain_track;
    if (t->gains & GAIN_ALBUM) return t->gain_album;
    if (t->gains & GAIN_SC) return t->gain_sc;
    return GAIN_NONE;
}

int audio_level_volume(int gain, int max)
{
    if (gain == GAIN_NONE) return max;
    int cut = gain + LEVEL_HEADROOM; /* hundredths of a dB, only ever <= 0 */
    if (cut >= 0) return max;
    if (cut < -4000) cut = -4000;
    /* max * 10^(cut / 2000): whole dB steps of 10^(-1/20), then the rest by its series */
    double f = 1.0;
    int db = -cut / 100, rest = -cut % 100;
    for (int i = 0; i < db; i++) f *= 0.89125093813374556; /* -1 dB */
    double x = -rest / 2000.0 * 2.30258509299404568, e = 1, term = 1; /* e^x, |x| < 0.12 */
    for (int i = 1; i < 8; i++) { term *= x / i; e += term; }
    f *= e;
    int v = (int)(max * f + 0.5);
    return v < 1 ? 1 : v;
}

int audio_rate_ok(int rate)
{
    static const int ok[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        if (ok[i] == rate)
            return 1;
    return 0;
}

int adts_parse(const uint8_t *p, int n, Adts *out)
{
    static const int rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    if (n < 7 || p[0] != 0xFF || (p[1] & 0xF6) != 0xF0)
        return -1;
    int sri = p[2] >> 2 & 15;
    if (sri >= 13)
        return -1;
    int header = p[1] & 1 ? 7 : 9;
    int frame = (p[3] & 3) << 11 | p[4] << 3 | p[5] >> 5;
    if (frame <= header)
        return -1;
    out->object = (p[2] >> 6) + 1;
    out->rate = rates[sri];
    out->channels = (p[2] & 1) << 2 | p[3] >> 6;
    out->header = header;
    out->frame = frame;
    return 0;
}

int adts_find(const uint8_t *p, int n)
{
    Adts a, b;
    for (int i = 0; i + 7 <= n; i++) {
        if (adts_parse(p + i, n - i, &a))
            continue;
        if (i + a.frame + 7 > n)
            return i; /* the next one is out of sight: this is the best there is */
        if (!adts_parse(p + i + a.frame, n - i - a.frame, &b) && b.rate == a.rate)
            return i;
    }
    return -1;
}
