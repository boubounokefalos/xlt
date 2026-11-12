/* XLT - MP3 playback.
 *
 * Same technique as psp5's player, which is tested on hardware:
 *  - set a compiled SDK version first, or sceMp3 only accepts 44.1 kHz;
 *  - load AVCODEC + MP3 with sceUtilityLoadModule ("already loaded" is fine);
 *  - feed the decoder from the file, play through the SRC channel (any rate),
 *    mono frames expanded to stereo;
 *  - just after a seek the decoder may return MP3_END for frames that lack
 *    data from the frame before (bit reservoir): that is not the end;
 *  - reopen the file if a read fails after sleep.
 * The player thread runs above the UI (0x14 < 0x20) and spends its time in
 * sceAudioSRCOutputBlocking, so the UI never makes the audio stutter.
 *
 * The UI thread owns the playlist copy. It only replaces it after the player
 * has acknowledged a stop, so the player never reads a list being freed.
 *
 * Songs play in s_order (0..n-1, or shuffled with the chosen one first);
 * the player thread owns the order's contents once a list is playing and
 * mixes or unmixes it when shuffle is switched (CMD_SHUFFLE).
 *
 * Where playback is (list, song, time, shuffle, repeat) is saved in cache/
 * (resume.c) on every song change, pause and every 20 s, and when the player
 * shuts down for a game, USB or exit, so music_resume() can carry on.
 *
 * AAC (.m4a, .aac) goes through the PSP's own decoder (sceAudiocodec, the way the
 * video player's sound does) and FLAC through flac.c on the CPU. Both are "alt"
 * tracks: s_alt says which, s_handle stays -1, and they hand the output blocks
 * of s_chunk samples (4096 if the hardware takes it): ~85 ms of sound, so a slow
 * Memory Stick read or a long FLAC block is hidden behind the one playing.
 */
/* Parts of this file (the MP3 engine (decoder setup, stream feeding, frame and VBR parsing, seeking, SRC output)) are adapted from psp5 by 08Weber,
 * https://github.com/08Weber/psp5, GPL-3.0. */
#include <pspkernel.h>
#include <pspaudio.h>
#include <pspmp3.h>
#include <pspaudiocodec.h>
#include <psputility.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "music.h"
#include "library.h"
#include "system.h"
#include "video.h"
#include "resume.h"
#include "bg.h"
#include "ui.h"
#include "audiofmt.h"
#include "gamecfg.h"
#include "flac.h"
#include "mp4.h"
#include "readwin.h"

#define MP3_END 0x80671402
#define ALREADY_LOADED 0x80020139
#define ALREADY_LOADED2 0x80111102
#define MAX_TRACKS 1000
#define MAX_SAMPLES 1152
#define QUEUE 16

typedef struct {
    char path[sizeof(((Entry *)0)->path)];
    char title[128];
    char artist[48];
} Track;

enum { CMD_PLAY, CMD_TOGGLE, CMD_NEXT, CMD_PREV, CMD_SEEK, CMD_STOP, CMD_SHUFFLE, CMD_QUIT };

/* UI thread */
static Track *s_list;
static int s_count;
static int s_ready;        /* modules loaded, resource initialised */
static int s_loaded_avcodec, s_loaded_mp3;
static SceUID s_thread = -1;
static int s_seq;          /* commands posted */
static int *s_order;       /* play order: positions -> tracks */
static int s_shuffle;      /* the switch (the player's copy follows by CMD_SHUFFLE) */
static unsigned s_seed;
static char s_cache[200];  /* <app>/cache, where the place is saved; "" before music_init */
static ResumeState s_saved; /* what music.txt says */

/* shared */
static volatile int s_q_cmd[QUEUE], s_q_arg[QUEUE], s_q_seq[QUEUE];
static volatile int s_q_head, s_q_tail;
static volatile int s_done_seq; /* last command the player finished */
static volatile int s_state = MUSIC_STOPPED;
static volatile int s_current = -1;  /* track index in s_list */
static volatile int s_pos = -1;      /* its position in s_order */
static volatile int s_repeat;        /* REPEAT_* (read by the player) */
static volatile int s_ended;         /* the list played to its end: nothing to resume */
static volatile int s_volume = PSP_AUDIO_VOLUME_MAX; /* lowered as the sleep timer runs out */
static u32 s_sleep_at;               /* now_ms() when the sleep timer stops the music, 0 = off */
/* Volume levelling: the playing song's loudness tags, and the volume they give
 * (worked out again if Settings changes the mode mid-song). Only ever a cut. */
static Id3Info s_gain_tags;
static int s_level_mode = -1, s_level = PSP_AUDIO_VOLUME_MAX;

static int out_volume(void)
{
    if (s_level_mode != g_cfg.level) {
        s_level_mode = g_cfg.level;
        int g = audio_gain(&s_gain_tags, s_level_mode);
        s_level = audio_level_volume(g, PSP_AUDIO_VOLUME_MAX);
        if (g != GAIN_NONE)
            video_note("level (by %s): gain %s%d.%02d dB -> volume %d of %d", s_level_mode == LEVEL_ALBUM ? "album" : "song",
                       g < 0 ? "-" : "+", (g < 0 ? -g : g) / 100, (g < 0 ? -g : g) % 100, s_level, PSP_AUDIO_VOLUME_MAX);
    }
    return (int)((long long)s_volume * s_level / PSP_AUDIO_VOLUME_MAX);
}

/* The new song's loudness tags (a small read; nothing found = full volume). */
static void read_gain(const char *path, uint64_t size)
{
    memset(&s_gain_tags, 0, sizeof(s_gain_tags));
    if (audio_tags(path, size, &s_gain_tags) != 0)
        memset(&s_gain_tags, 0, sizeof(s_gain_tags));
    s_level_mode = -1; /* worked out at the first block */
}
static int s_sleep_min;
static volatile int s_elapsed_ms, s_duration_ms;
static volatile int s_gen;

/* player thread */
static SceInt32 s_handle = -1;
static SceUID s_fd = -1;
static u32 s_size, s_start;
static int s_eof, s_rate, s_channels, s_spf, s_seek_base_ms, s_seek_grace;
static int s_src_rate, s_src_samples;
static u32 s_first_hdr;
static unsigned char s_toc[100];
static int s_has_toc;
/* psp5's tested sizes: the decoder double-buffers inside the PCM buffer */
static unsigned char s_mp3buf[32 * 1024] __attribute__((aligned(64)));
static unsigned char s_pcmbuf[1152 * 2 * 2 * 2] __attribute__((aligned(64)));
static short s_out[2][MAX_SAMPLES * 2] __attribute__((aligned(64)));
static int s_out_idx;

static u32 be32(const unsigned char *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void bump(void) { s_gen++; }

/* ------------------------------------------------------------- file/stream */

static void src_release(void)
{
    if (!s_src_rate)
        return;
    for (int i = 0; i < 100 && sceAudioSRCChRelease() < 0; i++) /* busy while the last buffer plays */
        sceKernelDelayThread(2000);
    s_src_rate = s_src_samples = 0;
}

static int src_reserve(int samples, int rate)
{
    if (s_src_rate == rate && s_src_samples == samples)
        return 1;
    src_release();
    if (sceAudioSRCChReserve(samples, rate, 2) < 0)
        return 0;
    s_src_rate = rate;
    s_src_samples = samples;
    return 1;
}

/* ------------------------------------------------------------- AAC, FLAC */

#define ALT_MAX 4096          /* samples per output block, if the hardware takes that many */
#define AAC_SAMPLES 1024      /* one AAC frame */
#define AAC_WINDOW (64 * 1024) /* a read every couple of seconds, never one per frame */
#define AAC_FRAME_MAX (8 * 1024)
#define ADTS_LOOK 4096
#define PCM_BUFS 4            /* the hardware plays one while the next is decoded */

typedef struct {
    unsigned long codec[65] __attribute__((aligned(64)));
    short pcm[PCM_BUFS][ALT_MAX * 2] __attribute__((aligned(64))); /* the CPU never reads these through the cache */
    uint8_t in[AAC_FRAME_MAX] __attribute__((aligned(64)));
    Mp4 m4a;
    int has_m4a, slot, errors;
    uint32_t sample;           /* the next one (M4A) */
    uint64_t adts_pos, adts_start;
    ReadWin rw;
    uint8_t *win;
    void *work;
} Aac;

static int s_alt;              /* AUDIO_AAC / M4A / FLAC while such a track is open, else 0 */
static int s_played;           /* samples output since the last seek */
static Aac *s_aac;
static Flac s_flac;
static int16_t *s_fpcm;        /* the decoded block, 16-bit stereo */
static int s_fhave, s_fat;     /* frames in it, and how far it has been used */
static short s_fout[2][ALT_MAX * 2] __attribute__((aligned(64)));
static int s_chunk;            /* samples per output block now */
static u32 s_fmax, s_rmax;     /* the slowest block to decode and the slowest read, in microseconds */
static u32 s_rtime;            /* all the reading so far (to tell it from decoding) */
static int s_dry, s_fresh;     /* times the output ran out; 1 until the first block after a start or seek */
static const char *volatile s_open_err; /* why a song was skipped, for the UI thread */

/* Reads for the alt decoders; a file closed by sleep is opened again. */
static int alt_read(void *ctx, uint64_t off, void *dst, uint32_t len)
{
    int got = -1;
    u32 t0 = sceKernelGetSystemTimeLow();
    if (sceIoLseek(s_fd, (SceOff)off, PSP_SEEK_SET) >= 0)
        got = sceIoRead(s_fd, dst, len);
    if (got < 0 && s_current >= 0) {
        sceIoClose(s_fd);
        s_fd = sceIoOpen(s_list[s_current].path, PSP_O_RDONLY, 0777);
        if (s_fd >= 0 && sceIoLseek(s_fd, (SceOff)off, PSP_SEEK_SET) >= 0)
            got = sceIoRead(s_fd, dst, len);
    }
    u32 t = sceKernelGetSystemTimeLow() - t0;
    s_rtime += t;
    if (t > s_rmax) s_rmax = t;
    return got;
}

static void alt_close(void)
{
    if (s_alt)
        video_note("%s: blocks of %d samples; ran dry %d times; slowest read %u ms, slowest FLAC decode %u ms",
                   s_alt == AUDIO_FLAC ? "flac" : "aac", s_chunk, s_dry, s_rmax / 1000, s_fmax / 1000);
    if (s_alt == AUDIO_FLAC) {
        flac_close(&s_flac);
        free(s_fpcm);
        s_fpcm = NULL;
    } else if (s_alt && s_aac) {
        if (s_aac->has_m4a)
            mp4_close(&s_aac->m4a);
        free(s_aac->win);
        free(s_aac->work);
        free(s_aac);
        s_aac = NULL;
    }
    s_alt = 0;
}

static int open_flac(void)
{
    int r = flac_open(&s_flac, alt_read, NULL, s_size);
    if (r != 0) {
        video_note("flac: open = %d", r);
        s_open_err = r == FLAC_ERR_RATE ? "its sample rate is too unusual" : "it isn't a FLAC file";
        return -1;
    }
    s_fpcm = malloc((size_t)s_flac.alloc_block * 4);
    if (!s_fpcm) {
        flac_close(&s_flac);
        return -1;
    }
    s_fhave = s_fat = 0;
    s_fmax = 0;
    s_rate = s_flac.out_rate;
    s_channels = 2;
    s_duration_ms = flac_duration_ms(&s_flac);
    video_note("flac: %d Hz, %d bit, %d ch, blocks up to %d", s_flac.rate, s_flac.bps, s_flac.channels, s_flac.max_block);
    return 0;
}

/* Finds the first frame and works out the length from the frames that follow. */
static int open_adts(Aac *a, int *object, int *rate)
{
    uint32_t avail = s_size - s_start, look = avail < ADTS_LOOK ? avail : ADTS_LOOK;
    if (look < 7 || readwin_get(&a->rw, s_start, look, a->in) != 0)
        return -1;
    int at = adts_find(a->in, (int)look);
    Adts h;
    if (at < 0 || adts_parse(a->in + at, (int)look - at, &h) != 0)
        return -1;
    a->adts_start = a->adts_pos = (uint64_t)s_start + (uint32_t)at;
    *object = h.object;
    *rate = h.rate;
    uint64_t pos = a->adts_start, bytes = 0;
    int frames = 0;
    uint8_t b[9];
    while (frames < 256 && readwin_get(&a->rw, pos, 9, b) == 0 && adts_parse(b, 9, &h) == 0) {
        pos += (uint32_t)h.frame;
        bytes += (uint32_t)h.frame;
        frames++;
    }
    if (frames && bytes && *rate)
        s_duration_ms = (int)(((uint64_t)s_size - a->adts_start) * frames / bytes * 1024 * 1000 / (uint32_t)*rate);
    return 0;
}

static int open_aac(int format)
{
    Aac *a = memalign(64, sizeof(*a));
    if (!a)
        return -1;
    memset(a, 0, sizeof(*a));
    s_aac = a;
    a->win = memalign(64, AAC_WINDOW);
    if (!a->win)
        goto fail;
    readwin_init(&a->rw, a->win, AAC_WINDOW, alt_read, NULL);
    int object = 0, rate = 0;
    s_duration_ms = 0;
    if (format == AUDIO_M4A) {
        const char *why = "";
        if (mp4_open_audio(&a->m4a, s_list[s_current].path, &why) != 0) {
            video_note("m4a: %s", why);
            s_open_err = why;
            goto fail;
        }
        a->has_m4a = 1;
        object = a->m4a.aac_object;
        rate = a->m4a.sample_rate;
        s_duration_ms = a->m4a.duration_ms ? (int)a->m4a.duration_ms : (int)mp4_time_ms(&a->m4a.audio, a->m4a.audio.count);
    } else if (open_adts(a, &object, &rate) != 0) {
        s_open_err = "it isn't an AAC file";
        goto fail;
    }
    if (object != 2 || !audio_rate_ok(rate)) {
        video_note("aac: object %d at %d Hz can't be played", object, rate);
        s_open_err = object != 2 ? "only plain AAC (LC) is supported" : "its sample rate is too unusual";
        goto fail;
    }
    sceKernelDcacheWritebackInvalidateRange(a->pcm, sizeof(a->pcm));
    int r = sceAudiocodecCheckNeedMem(a->codec, PSP_CODEC_AAC);
    video_note("aac: CheckNeedMem = %08X, %lu bytes", r, a->codec[4]);
    if (r < 0 || !a->codec[4] || !(a->work = memalign(64, a->codec[4])))
        goto fail;
    a->codec[3] = (unsigned long)a->work;
    a->codec[10] = (unsigned long)rate;
    r = sceAudiocodecInit(a->codec, PSP_CODEC_AAC);
    video_note("aac: Init(%d Hz) = %08X", rate, r);
    if (r < 0) {
        s_open_err = "the AAC decoder didn't start";
        goto fail;
    }
    s_rate = rate;
    s_channels = 2;
    return 0;
fail:
    s_alt = format;
    alt_close();
    return -1;
}

static int open_alt(int format)
{
    s_played = 0;
    s_dry = 0;
    s_fresh = 1;
    s_fmax = s_rmax = 0;
    int r = format == AUDIO_FLAC ? open_flac() : open_aac(format);
    if (r == 0)
        s_alt = format;
    return r;
}

/* The next frame into a->in. Its size, 0 at the end of the song, -1 if it can't be read. */
static int aac_frame(Aac *a)
{
    uint32_t size;
    if (a->has_m4a) {
        if (a->sample >= a->m4a.audio.count)
            return 0;
        size = mp4_size(&a->m4a.audio, a->sample);
        if (!size || size > AAC_FRAME_MAX || readwin_get(&a->rw, mp4_offset(&a->m4a.audio, a->sample), size, a->in) != 0)
            return -1;
        a->sample++;
    } else {
        uint8_t b[9];
        Adts h;
        if (a->adts_pos + 7 >= s_size || readwin_get(&a->rw, a->adts_pos, 9, b) != 0)
            return 0;
        if (adts_parse(b, 9, &h) != 0)
            return -1;
        size = (uint32_t)(h.frame - h.header);
        if (size > AAC_FRAME_MAX || a->adts_pos + (uint32_t)h.frame > s_size ||
            readwin_get(&a->rw, a->adts_pos + (uint32_t)h.header, size, a->in) != 0)
            return 0; /* cut off: the end, as far as anyone can tell */
        a->adts_pos += (uint32_t)h.frame;
    }
    sceKernelDcacheWritebackRange(a->in, size); /* the decoder reads RAM directly */
    return (int)size;
}

static void note_played(int samples)
{
    s_played += samples;
    if (s_rate > 0)
        s_elapsed_ms = s_seek_base_ms + (int)((long long)s_played * 1000 / s_rate);
}

/* The output channel with the longest blocks it accepts (tried once per rate). */
static int alt_reserve(void)
{
    if (s_chunk && s_src_rate == s_rate && s_src_samples == s_chunk)
        return 1;
    static const int sizes[] = {ALT_MAX, 2048, AAC_SAMPLES};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        if (src_reserve(sizes[i], s_rate)) {
            s_chunk = sizes[i];
            return 1;
        }
    s_chunk = 0;
    return 0;
}

/* Hands a block over. A call that doesn't have to wait means the one before
 * had already finished: the sound ran dry in between. */
static void alt_output(short *out)
{
    u32 t0 = sceKernelGetSystemTimeLow();
    sceAudioSRCOutputBlocking(out_volume(), out);
    if (!s_fresh && sceKernelGetSystemTimeLow() - t0 < 500)
        s_dry++;
    s_fresh = 0;
}

/* 1 = continue, 0 = end of track, -1 = error */
static int play_aac(void)
{
    Aac *a = s_aac;
    if (!alt_reserve())
        return -1;
    short *out = a->pcm[a->slot];
    int got = 0, end = 0;
    while (got < s_chunk) { /* the decoder writes one frame at a time into the block */
        int size = aac_frame(a);
        if (size <= 0) {
            end = size;
            break;
        }
        short *at = out + got * 2;
        a->codec[6] = (unsigned long)a->in;
        a->codec[7] = (unsigned long)size;
        a->codec[8] = (unsigned long)at;
        a->codec[9] = AAC_SAMPLES * 4;
        int r = sceAudiocodecDecode(a->codec, PSP_CODEC_AAC);
        if (r < 0) {
            if (a->errors++ < 5) video_note("aac: Decode = %08X", r);
            memset((void *)((u32)at | 0x40000000u), 0, AAC_SAMPLES * 4); /* silence, uncached */
        }
        got += AAC_SAMPLES;
    }
    if (!got)
        return end;
    if (got < s_chunk)
        memset((void *)((u32)(out + got * 2) | 0x40000000u), 0, (size_t)(s_chunk - got) * 4);
    alt_output(out);
    a->slot = (a->slot + 1) % PCM_BUFS;
    note_played(got);
    return 1;
}

static int play_flac(void)
{
    if (!alt_reserve())
        return -1;
    short *o = s_fout[s_out_idx];
    int fill = 0;
    while (fill < s_chunk) {
        if (s_fat >= s_fhave) {
            u32 t0 = sceKernelGetSystemTimeLow(), r0 = s_rtime;
            int b = flac_decode(&s_flac);
            if (b <= 0)
                break; /* the end of the song */
            s_fhave = flac_output(&s_flac, s_fpcm);
            s_fat = 0;
            u32 t = sceKernelGetSystemTimeLow() - t0 - (s_rtime - r0); /* the reading is counted on its own */
            if (t > s_fmax) s_fmax = t;
        }
        int n = s_fhave - s_fat;
        if (n > s_chunk - fill) n = s_chunk - fill;
        memcpy(o + fill * 2, s_fpcm + s_fat * 2, (size_t)n * 4);
        fill += n;
        s_fat += n;
    }
    if (!fill)
        return 0;
    memset(o + fill * 2, 0, (size_t)(s_chunk - fill) * 4);
    sceKernelDcacheWritebackRange(o, (size_t)s_chunk * 4);
    s_out_idx ^= 1;
    alt_output(o);
    note_played(fill);
    return 1;
}

/* 0 = done, -1 = can't (go to the next track). */
static int alt_seek(int ms)
{
    int base = ms;
    if (s_alt == AUDIO_FLAC) {
        if (flac_seek(&s_flac, (uint64_t)ms * (uint32_t)s_flac.rate / 1000) != 0)
            return -1;
        s_fhave = s_fat = 0;
    } else if (s_aac->has_m4a) {
        s_aac->sample = mp4_sample_at(&s_aac->m4a.audio, (uint32_t)ms);
        base = (int)mp4_time_ms(&s_aac->m4a.audio, s_aac->sample);
    } else {
        Aac *a = s_aac;
        uint64_t span = (uint64_t)s_size - a->adts_start;
        uint64_t pos = a->adts_start + span * (uint32_t)ms / (uint32_t)s_duration_ms;
        uint32_t left = (uint32_t)(s_size - pos), look = left < ADTS_LOOK ? left : ADTS_LOOK;
        if (look < 7 || readwin_get(&a->rw, pos, look, a->in) != 0)
            return -1;
        int at = adts_find(a->in, (int)look);
        if (at < 0)
            return -1;
        a->adts_pos = pos + (uint32_t)at;
    }
    s_played = 0;
    s_fresh = 1; /* the gap a seek makes isn't the decoder's */
    s_seek_base_ms = base;
    s_elapsed_ms = base;
    return 0;
}

static void close_track(void)
{
    alt_close();
    if (s_handle >= 0) {
        sceMp3ReleaseMp3Handle(s_handle);
        s_handle = -1;
    }
    if (s_fd >= 0) {
        sceIoClose(s_fd);
        s_fd = -1;
    }
}

static u32 id3v2_size(SceUID fd)
{
    unsigned char h[10];
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    if (sceIoRead(fd, h, 10) != 10 || memcmp(h, "ID3", 3))
        return 0;
    u32 n = (h[6] & 0x7F) << 21 | (h[7] & 0x7F) << 14 | (h[8] & 0x7F) << 7 | (h[9] & 0x7F);
    return 10 + n + ((h[5] & 0x10) ? 10 : 0);
}

static int feed(void)
{
    unsigned char *dst;
    SceInt32 towrite, srcpos;
    if (s_eof)
        return 0;
    if (sceMp3GetInfoToAddStreamData(s_handle, &dst, &towrite, &srcpos) < 0)
        return -1;
    if (towrite <= 0)
        return 0;
    int got = -1;
    if (sceIoLseek32(s_fd, srcpos, PSP_SEEK_SET) >= 0)
        got = sceIoRead(s_fd, dst, towrite);
    if (got < 0 && s_current >= 0) { /* the file is closed by sleep: reopen once */
        sceIoClose(s_fd);
        s_fd = sceIoOpen(s_list[s_current].path, PSP_O_RDONLY, 0777);
        if (s_fd >= 0 && sceIoLseek32(s_fd, srcpos, PSP_SEEK_SET) >= 0)
            got = sceIoRead(s_fd, dst, towrite);
    }
    if (got < 0)
        return -1;
    if (got == 0) {
        s_eof = 1;
        return 0;
    }
    return sceMp3NotifyAddStreamData(s_handle, got) < 0 ? -1 : 0;
}

static const int MP3_RATES[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};

static int frame_len(u32 h)
{
    static const short kbps1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const short kbps2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    int ver = (h >> 19) & 3, layer = (h >> 17) & 3, bi = (h >> 12) & 15, ri = (h >> 10) & 3;
    if ((h & 0xFFE00000) != 0xFFE00000 || ver == 1 || layer != 1 || ri == 3)
        return 0;
    int kbps = ver == 3 ? kbps1[bi] : kbps2[bi], rate = MP3_RATES[ver][ri];
    if (!kbps)
        return 0;
    return (ver == 3 ? 144 : 72) * kbps * 1000 / rate + ((h >> 9) & 1);
}

/* Duration in ms from a Xing/Info or VBRI header (0 if absent); keeps the TOC for seeking. */
static int parse_first_frame(u32 start)
{
    unsigned char b[192];
    s_first_hdr = 0;
    s_has_toc = 0;
    sceIoLseek32(s_fd, start, PSP_SEEK_SET);
    if (sceIoRead(s_fd, b, sizeof(b)) != sizeof(b))
        return 0;
    u32 h = be32(b);
    if (!frame_len(h))
        return 0;
    s_first_hdr = h;
    int ver = (h >> 19) & 3, ri = (h >> 10) & 3, mono = ((h >> 6) & 3) == 3;
    int rate = MP3_RATES[ver][ri], spf = ver == 3 ? 1152 : 576;
    s_spf = spf;
    int off = ver == 3 ? (mono ? 21 : 36) : (mono ? 13 : 21);
    u32 frames = 0;
    if (!memcmp(b + off, "Xing", 4) || !memcmp(b + off, "Info", 4)) {
        u32 flags = be32(b + off + 4);
        int p = off + 8;
        if (flags & 1) { frames = be32(b + p); p += 4; }
        if (flags & 2) p += 4;
        if ((flags & 4) && p + 100 <= (int)sizeof(b)) {
            memcpy(s_toc, b + p, 100);
            s_has_toc = 1;
        }
    } else if (!memcmp(b + 36, "VBRI", 4)) {
        frames = be32(b + 36 + 14);
    }
    return frames && rate ? (int)((unsigned long long)frames * spf * 1000 / rate) : 0;
}

static int start_decoder(u32 offset)
{
    if (s_handle >= 0) {
        sceMp3ReleaseMp3Handle(s_handle);
        s_handle = -1;
    }
    SceMp3InitArg args;
    memset(&args, 0, sizeof(args));
    args.mp3StreamStart = offset;
    args.mp3StreamEnd = s_size;
    args.mp3Buf = s_mp3buf;
    args.mp3BufSize = sizeof(s_mp3buf);
    args.pcmBuf = s_pcmbuf;
    args.pcmBufSize = sizeof(s_pcmbuf);
    s_handle = sceMp3ReserveMp3Handle(&args);
    if (s_handle < 0)
        return -1;
    s_eof = 0;
    if (feed() < 0 || sceMp3Init(s_handle) < 0) {
        sceMp3ReleaseMp3Handle(s_handle);
        s_handle = -1;
        return -1;
    }
    sceMp3SetLoopNum(s_handle, 0);
    return 0;
}

static int open_track(int i)
{
    close_track();
    if (i < 0 || i >= s_count)
        return -1;
    s_fd = sceIoOpen(s_list[i].path, PSP_O_RDONLY, 0777);
    if (s_fd < 0)
        return -1;
    s_size = sceIoLseek32(s_fd, 0, PSP_SEEK_END);
    s_start = id3v2_size(s_fd);
    if (s_start >= s_size)
        s_start = 0;
    s_current = i;
    read_gain(s_list[i].path, (uint64_t)s_size);
    int format = audio_format(s_list[i].path);
    if (format != AUDIO_MP3) {
        s_elapsed_ms = s_seek_base_ms = 0;
        s_seek_grace = 0;
        if (open_alt(format) < 0) {
            close_track();
            return -1;
        }
        return 0;
    }
    if (start_decoder(s_start) < 0) {
        close_track();
        return -1;
    }
    s_rate = sceMp3GetSamplingRate(s_handle);
    s_channels = sceMp3GetMp3ChannelNum(s_handle);
    if (s_rate <= 0 || s_channels < 1 || s_channels > 2) {
        close_track();
        return -1;
    }
    s_spf = 0;
    int dur = parse_first_frame(s_start);
    if (dur <= 0) { /* CBR: from the bitrate */
        int kbps = sceMp3GetBitRate(s_handle);
        dur = kbps > 0 ? (int)((unsigned long long)(s_size - s_start) * 8 / kbps) : 0;
    }
    s_duration_ms = dur;
    s_elapsed_ms = s_seek_base_ms = 0;
    s_seek_grace = 0;
    return 0;
}

/* First frame at or after pos that matches the track's first frame (and is followed by one). */
static u32 find_frame(u32 pos)
{
    static unsigned char b[4096];
    sceIoLseek32(s_fd, pos, PSP_SEEK_SET);
    int n = sceIoRead(s_fd, b, sizeof(b));
    const u32 same = 0x001E0C00; /* version, layer, sample rate */
    for (int i = 0; i + 4 <= n; i++) {
        u32 h = be32(b + i);
        int len = frame_len(h);
        if (!len || (s_first_hdr && ((h ^ s_first_hdr) & same)))
            continue;
        if (i + len + 4 <= n) {
            u32 next = be32(b + i + len);
            if (!frame_len(next) || ((next ^ h) & same))
                continue;
        }
        return pos + i;
    }
    return pos;
}

/* 0 = done, -1 = past the end (go to the next track). */
static int seek_to(int ms)
{
    if (s_alt) {
        if (s_duration_ms <= 0)
            return 0;
        if (ms >= s_duration_ms - 500)
            return -1;
        return alt_seek(ms < 0 ? 0 : ms);
    }
    if (s_handle < 0 || s_fd < 0 || s_duration_ms <= 0)
        return 0;
    if (ms >= s_duration_ms - 500)
        return -1;
    if (ms < 0)
        ms = 0;
    u32 span = s_size - s_start, pos;
    if (s_has_toc) { /* VBR: the Xing table maps each percent of time to a byte position */
        int pct10 = (int)((long long)ms * 1000 / s_duration_ms); /* tenths of a percent */
        int i = pct10 / 10 < 99 ? pct10 / 10 : 99, frac = pct10 % 10;
        int a = s_toc[i], b = i < 99 ? s_toc[i + 1] : 256;
        pos = s_start + (u32)(((long long)(a * 10 + (b - a) * frac) * span) / 2560);
    } else {
        pos = s_start + (u32)((long long)ms * span / s_duration_ms);
    }
    u32 frame = ms > 0 ? find_frame(pos) : s_start;
    if (start_decoder(frame) < 0) {
        if (start_decoder(s_start) < 0)
            return -1;
        frame = s_start;
    }
    s_seek_base_ms = frame == s_start ? 0 : ms;
    s_elapsed_ms = s_seek_base_ms;
    s_seek_grace = 8;
    return 0;
}

/* 1 = continue, 0 = end of track, -1 = error */
static int play_frame(void)
{
    if (s_alt)
        return s_alt == AUDIO_FLAC ? play_flac() : play_aac();
    if (sceMp3CheckStreamDataNeeded(s_handle) > 0 && feed() < 0)
        return -1;
    SceShort16 *pcm;
    int bytes = sceMp3Decode(s_handle, &pcm);
    if (bytes <= 0 && s_seek_grace > 0) {
        s_seek_grace--;
        return 1;
    }
    if (bytes == 0 || (u32)bytes == MP3_END)
        return 0;
    if (bytes < 0)
        return -1;
    int out_ch = s_channels; /* the decoder may hand mono files back as stereo */
    if (s_spf && bytes == s_spf * 4) out_ch = 2;
    else if (s_spf && bytes == s_spf * 2) out_ch = 1;
    else if (bytes / (2 * out_ch) > MAX_SAMPLES) out_ch = 2;
    int samples = bytes / (2 * out_ch);
    if (samples <= 0 || samples > MAX_SAMPLES)
        return -1;
    int want = samples > 576 ? 1152 : 576;
    if (s_src_rate == s_rate && s_src_samples >= samples)
        want = s_src_samples;
    if (!src_reserve(want, s_rate))
        return -1;
    void *out = pcm;
    if (out_ch == 1 || samples != s_src_samples) {
        short *o = s_out[s_out_idx];
        s_out_idx ^= 1;
        for (int i = 0; i < samples; i++) {
            if (out_ch == 1) {
                o[i * 2] = o[i * 2 + 1] = pcm[i];
            } else {
                o[i * 2] = pcm[i * 2];
                o[i * 2 + 1] = pcm[i * 2 + 1];
            }
        }
        memset(o + samples * 2, 0, (s_src_samples - samples) * 4);
        out = o;
    }
    sceAudioSRCOutputBlocking(out_volume(), out);
    int sum = sceMp3GetSumDecodedSample(s_handle);
    if (sum >= 0 && s_rate > 0)
        s_elapsed_ms = s_seek_base_ms + (int)((long long)sum * 1000 / s_rate);
    if (s_seek_grace > 0)
        s_seek_grace--;
    return 1;
}

/* ------------------------------------------------------------ the player */

static void stop_playback(void)
{
    close_track();
    src_release();
    s_state = MUSIC_STOPPED;
    s_current = -1;
    s_pos = -1;
    s_elapsed_ms = s_duration_ms = 0;
    bump();
}

/* Opens the song at position pos of the play order, skipping files that
 * won't play (at most a full round). */
static void start_pos(int pos)
{
    if (pos < 0 || pos >= s_count) {
        stop_playback();
        return;
    }
    for (int tries = 0; tries < s_count; tries++, pos = (pos + 1) % s_count) {
        if (open_track(s_order[pos]) == 0) {
            s_pos = pos;
            s_state = MUSIC_PLAYING;
            bump();
            return;
        }
    }
    stop_playback();
}

/* ended: the song finished by itself (repeat one plays it again). */
static void advance(int ended)
{
    int next = order_next(s_pos, s_count, s_repeat, ended);
    if (next < 0) {
        stop_playback(); /* the end of the list */
        s_ended = 1;
    } else {
        start_pos(next);
    }
}

/* Shuffle switched while a list is loaded: mix (the current song first) or put it back in order. */
static void reorder(int on)
{
    if (!s_order || s_count <= 0)
        return;
    int cur = s_current;
    play_order(s_order, s_count, on, cur >= 0 ? cur : 0, &s_seed);
    if (cur >= 0)
        s_pos = on ? 0 : cur;
    bump();
}

static void handle(int cmd, int arg)
{
    switch (cmd) {
    case CMD_PLAY: start_pos(arg); break;
    case CMD_TOGGLE:
        s_fresh = 1; /* a pause empties the output: not a dry spell */
        if (s_state == MUSIC_PLAYING) s_state = MUSIC_PAUSED;
        else if (s_state == MUSIC_PAUSED) s_state = MUSIC_PLAYING;
        bump();
        break;
    case CMD_NEXT:
        if (s_current >= 0) advance(0);
        break;
    case CMD_PREV:
        if (s_current < 0) break;
        if (s_elapsed_ms > 3000) start_pos(s_pos); /* back to the start of this one */
        else start_pos(order_prev(s_pos, s_count, s_repeat));
        break;
    case CMD_SEEK:
        if (s_current >= 0 && seek_to(s_elapsed_ms + arg) < 0)
            advance(0);
        break;
    case CMD_STOP: stop_playback(); break;
    case CMD_SHUFFLE: reorder(arg); break;
    }
}

static int player_thread(SceSize args, void *argp)
{
    int failures = 0;
    for (;;) {
        while (s_q_tail != s_q_head) {
            int t = s_q_tail, cmd = s_q_cmd[t];
            if (cmd == CMD_QUIT) {
                stop_playback();
                s_done_seq = s_q_seq[t];
                return 0;
            }
            handle(cmd, s_q_arg[t]);
            s_done_seq = s_q_seq[t];
            s_q_tail = (t + 1) % QUEUE;
            failures = 0;
        }
        if (s_state != MUSIC_PLAYING || (s_handle < 0 && !s_alt)) {
            sceKernelDelayThread(10000);
            continue;
        }
        int r = play_frame();
        if (r > 0) {
            failures = 0;
            continue;
        }
        if (r < 0 && ++failures >= 8) { /* broken files in a row: give up */
            stop_playback();
            continue;
        }
        advance(r == 0); /* a broken file is skipped, even on repeat one */
    }
}

/* ---------------------------------------------------------------- the UI */

static int post(int cmd, int arg)
{
    if (s_thread < 0)
        return -1;
    int h = s_q_head, n = (h + 1) % QUEUE;
    if (n == s_q_tail)
        return -1; /* full: dropped */
    s_q_cmd[h] = cmd;
    s_q_arg[h] = arg;
    s_q_seq[h] = ++s_seq;
    s_q_head = n;
    return s_seq;
}

static void wait_done(int seq)
{
    for (int i = 0; seq > 0 && s_done_seq < seq && i < 500; i++)
        sceKernelDelayThread(1000);
}

static int load_modules(void)
{
    if (s_ready)
        return 0;
    if (sceKernelGetCompiledSdkVersion() == 0)
        sceKernelSetCompiledSdkVersion(0x06060010); /* or sceMp3 only takes 44.1 kHz */
    int r = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    s_loaded_avcodec = r >= 0;
    if (r < 0 && (u32)r != ALREADY_LOADED && (u32)r != ALREADY_LOADED2)
        return -1;
    r = sceUtilityLoadModule(PSP_MODULE_AV_MP3);
    s_loaded_mp3 = r >= 0;
    if (r < 0 && (u32)r != ALREADY_LOADED && (u32)r != ALREADY_LOADED2)
        goto fail;
    if (sceMp3InitResource() < 0)
        goto fail;
    s_q_head = s_q_tail = 0;
    s_done_seq = s_seq;
    s_thread = sceKernelCreateThread("xl_music", player_thread, 0x14, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (s_thread < 0 || sceKernelStartThread(s_thread, 0, NULL) < 0) {
        if (s_thread >= 0) sceKernelDeleteThread(s_thread);
        s_thread = -1;
        sceMp3TermResource();
        goto fail;
    }
    s_ready = 1;
    return 0;
fail:
    if (s_loaded_mp3) sceUtilityUnloadModule(PSP_MODULE_AV_MP3);
    if (s_loaded_avcodec) sceUtilityUnloadModule(PSP_MODULE_AV_AVCODEC);
    s_loaded_mp3 = s_loaded_avcodec = 0;
    return -1;
}

/* ------------------------------------------------------- the saved place */

static u32 now_ms(void) { return sceKernelGetSystemTimeLow() / 1000; }

static int read_text(const char *path, char **out)
{
    *out = NULL;
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return -1;
    int size = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    char *b = size >= 0 && size < 1024 * 1024 ? malloc(size + 1) : NULL;
    int n = b ? sceIoRead(fd, b, size) : -1;
    sceIoClose(fd);
    if (n < 0) {
        free(b);
        return -1;
    }
    b[n] = 0;
    *out = b;
    return n;
}

static void write_text(const char *path, const char *text, int len)
{
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return;
    sceIoWrite(fd, text, len);
    sceIoClose(fd);
}

typedef struct {
    char path[220];
    char text[512];
    int len;
} StateArg;

static void job_write_state(void *arg)
{
    const StateArg *a = arg;
    write_text(a->path, a->text, a->len);
}

/* Writes s_saved: on the worker, or right away (sync) before the stick goes away. */
static void save_state(int sync)
{
    if (!s_cache[0])
        return;
    static StateArg a;
    snprintf(a.path, sizeof(a.path), "%s/music.txt", s_cache);
    a.len = resume_write_state(a.text, sizeof(a.text), &s_saved);
    if (a.len < 0)
        return;
    if (sync) write_text(a.path, a.text, a.len);
    else bg_post(job_write_state, &a, sizeof(a));
}

typedef struct {
    char *text;
    int len;
    char path[220];
} ListArg;

static void job_write_list(void *arg)
{
    const ListArg *a = arg;
    write_text(a->path, a->text, a->len);
    free(a->text);
}

/* The new list, for music_resume (on the worker). */
static void save_list(void)
{
    if (!s_cache[0])
        return;
    int cap = s_count * (int)(sizeof(Track) + 4) + 1;
    ListArg a;
    a.text = malloc(cap);
    if (!a.text)
        return;
    a.len = 0;
    for (int i = 0; i < s_count; i++) {
        int n = resume_write_song(a.text + a.len, cap - a.len, s_list[i].path, s_list[i].title, s_list[i].artist);
        if (n > 0) a.len += n;
    }
    snprintf(a.path, sizeof(a.path), "%s/music_list.txt", s_cache);
    bg_post(job_write_list, &a, sizeof(a)); /* the worker writes, then frees the text */
}

/* s_saved from what is playing now. */
static void note_place(void)
{
    int pos = s_pos;
    if (!s_list || !s_order || pos < 0 || pos >= s_count)
        return;
    int t = s_order[pos];
    s_saved.track = t;
    s_saved.ms = s_elapsed_ms;
    snprintf(s_saved.title, sizeof(s_saved.title), "%s", s_list[t].title);
    snprintf(s_saved.artist, sizeof(s_saved.artist), "%s", s_list[t].artist);
}

void music_init(const char *app_dir)
{
    snprintf(s_cache, sizeof(s_cache), "%.180scache", app_dir);
    s_seed = sceKernelGetSystemTimeLow();
    char path[220], *text;
    snprintf(path, sizeof(path), "%s/music.txt", s_cache);
    if (read_text(path, &text) >= 0) {
        resume_parse_state(text, &s_saved);
        free(text);
    } else {
        resume_parse_state("", &s_saved);
    }
    s_shuffle = s_saved.shuffle;
    s_repeat = s_saved.repeat;
}

/* The sleep timer: the last 10 s fade out, then the music stops (its place kept). */
static void sleep_tick(void)
{
    if (!s_sleep_at)
        return;
    int left = (int)(s_sleep_at - now_ms());
    if (s_state == MUSIC_STOPPED || left <= 0) {
        if (s_state != MUSIC_STOPPED) {
            note_place();
            save_state(0);
            post(CMD_STOP, 0);
            ui_toast("Sleep timer: music stopped");
        }
        s_sleep_at = 0;
        s_sleep_min = 0;
        s_volume = PSP_AUDIO_VOLUME_MAX;
        bump();
        return;
    }
    s_volume = left < 10000 ? PSP_AUDIO_VOLUME_MAX * left / 10000 : PSP_AUDIO_VOLUME_MAX;
}

static const int SLEEP_STEPS[] = {0, 15, 30, 45, 60, 90};

int music_sleep_cycle(void)
{
    int i = 0;
    while (i < 5 && SLEEP_STEPS[i] != s_sleep_min) i++;
    s_sleep_min = SLEEP_STEPS[(i + 1) % 6];
    s_sleep_at = s_sleep_min ? now_ms() + (u32)s_sleep_min * 60000u : 0;
    if (!s_sleep_at) s_volume = PSP_AUDIO_VOLUME_MAX;
    bump();
    return s_sleep_min;
}

int music_sleep_left(void)
{
    if (!s_sleep_at)
        return 0;
    int left = (int)(s_sleep_at - now_ms());
    return left > 0 ? (left + 59999) / 60000 : 0;
}

void music_poll(void)
{
    static int last_pos = -2, last_state = -1;
    static u32 last_save;
    if (s_open_err) { /* a song that couldn't be played was passed over */
        char msg[80];
        snprintf(msg, sizeof(msg), "Skipped a song: %s", s_open_err);
        s_open_err = NULL;
        ui_toast(msg);
    }
    if (!s_cache[0])
        return;
    sleep_tick();
    if (s_ended) { /* played to the end: next time starts fresh */
        s_ended = 0;
        s_saved.track = -1;
        s_saved.ms = 0;
        save_state(0);
        last_pos = -1;
        last_state = s_state;
        return;
    }
    int st = s_state, pos = s_pos;
    if (pos < 0)
        return;
    u32 t = now_ms();
    if (pos != last_pos || st != last_state || (st == MUSIC_PLAYING && t - last_save > 20000)) {
        last_pos = pos;
        last_state = st;
        last_save = t;
        note_place();
        save_state(0);
    }
}

/* ------------------------------------------------------------ new lists */

/* Stops the player and makes room for a list of n songs (at most MAX_TRACKS).
 * Returns the count kept, or 0. */
static int begin_list(int n)
{
    if (video_is_open()) /* the player has the ME and the audio: songs would only fail one after another */
        return 0;
    video_preview_stop(1); /* an MP4 preview has the ME booted its own way */
    if (n <= 0 || load_modules() != 0)
        return 0;
    wait_done(post(CMD_STOP, 0)); /* the player lets go of the old list */
    free(s_list);
    free(s_order);
    s_count = n < MAX_TRACKS ? n : MAX_TRACKS;
    s_list = malloc(sizeof(Track) * s_count);
    s_order = malloc(sizeof(int) * s_count);
    if (!s_list || !s_order) {
        free(s_list);
        free(s_order);
        s_list = NULL;
        s_order = NULL;
        s_count = 0;
    }
    return s_count;
}

/* Plays the filled list from track `index` (in the shuffle order if it's on). */
static void go(int index)
{
    play_order(s_order, s_count, s_shuffle, index, &s_seed);
    power_hold(222); /* steady clock while music plays; released when it stops */
    post(CMD_PLAY, s_shuffle ? 0 : index);
}

/* Replaces the playlist (after the player let go of the old one) and plays it from `index`. */
typedef void (*FillFn)(Track *t, int i, const void *ctx);

static void play_list(int n, int index, FillFn fill, const void *ctx)
{
    if (index < 0 || index >= n || !begin_list(n))
        return;
    /* More tracks than the cap: copy a window of MAX_TRACKS around the chosen one. */
    int first = 0;
    if (n > s_count) {
        first = index - s_count / 2;
        if (first < 0) first = 0;
        if (first + s_count > n) first = n - s_count;
    }
    for (int i = 0; i < s_count; i++)
        fill(&s_list[i], first + i, ctx);
    go(index - first);
    save_list();
}

static void fill_view(Track *t, int i, const void *ctx)
{
    const Entry *e = library_get(VIEW_MUSIC, i);
    snprintf(t->path, sizeof(t->path), "%s", e->path);
    snprintf(t->title, sizeof(t->title), "%s", e->title);
    snprintf(t->artist, sizeof(t->artist), "%s", e->artist);
}

void music_play_view(int index)
{
    play_list(library_count(VIEW_MUSIC), index, fill_view, NULL);
}

static void fill_files(Track *t, int i, const void *ctx)
{
    const char *const *paths = ctx;
    const char *p = paths[i], *name = strrchr(p, '/');
    name = name ? name + 1 : p;
    const char *dot = strrchr(name, '.');
    snprintf(t->path, sizeof(t->path), "%s", p);
    snprintf(t->title, sizeof(t->title), "%.*s", dot ? (int)(dot - name) : (int)strlen(name), name);
    t->artist[0] = 0;
    /* the library knows the tags if the file is in MUSIC/ */
    int k = library_find(VIEW_MUSIC, p);
    if (k >= 0) {
        const Entry *e = library_get(VIEW_MUSIC, k);
        snprintf(t->title, sizeof(t->title), "%s", e->title);
        snprintf(t->artist, sizeof(t->artist), "%s", e->artist);
    }
}

void music_play_files(const char *const *paths, int n, int index)
{
    play_list(n, index, fill_files, paths);
}

static void fill_entries(Track *t, int i, const void *ctx)
{
    const Entry *e = (const Entry *)ctx + i;
    snprintf(t->path, sizeof(t->path), "%s", e->path);
    snprintf(t->title, sizeof(t->title), "%s", e->title);
    snprintf(t->artist, sizeof(t->artist), "%s", e->artist);
}

void music_play_entries(const Entry *list, int n, int index)
{
    play_list(n, index, fill_entries, list);
}

typedef struct { Track *t; int n, cap; } Loaded;

static void add_loaded(const char *path, const char *title, const char *artist, void *ctx)
{
    Loaded *l = ctx;
    if (l->n >= l->cap)
        return;
    Track *t = &l->t[l->n++];
    snprintf(t->path, sizeof(t->path), "%s", path);
    snprintf(t->title, sizeof(t->title), "%s", title);
    snprintf(t->artist, sizeof(t->artist), "%s", artist);
}

int music_can_resume(const char **title, const char **artist)
{
    if (!s_cache[0] || s_state != MUSIC_STOPPED || s_saved.track < 0)
        return 0;
    if (title) *title = s_saved.title;
    if (artist) *artist = s_saved.artist;
    return 1;
}

int music_resume(void)
{
    if (!music_can_resume(NULL, NULL))
        return -1;
    char path[220], *text;
    snprintf(path, sizeof(path), "%s/music_list.txt", s_cache);
    bg_flush(); /* a list still being written goes first */
    int len = read_text(path, &text);
    int n = len > 0 ? resume_parse_list(text, len, NULL, NULL) : 0;
    if (n <= s_saved.track || !begin_list(n)) {
        free(text);
        if (n <= s_saved.track) {
            s_saved.track = -1; /* the list is gone or shorter: nothing to go back to */
            save_state(0);
        }
        return -1;
    }
    Loaded l = {s_list, 0, s_count};
    resume_parse_list(text, len, add_loaded, &l);
    free(text);
    s_count = l.n;
    int ms = s_saved.ms - 2000; /* a moment before, to pick up the thread */
    go(s_saved.track);
    if (ms > 3000)
        post(CMD_SEEK, ms);
    return 0;
}

void music_set_shuffle(int on)
{
    s_shuffle = on != 0;
    if (s_list && s_thread >= 0)
        post(CMD_SHUFFLE, s_shuffle);
    s_saved.shuffle = s_shuffle;
    save_state(0);
    bump();
}

int music_shuffle(void) { return s_shuffle; }

void music_set_repeat(int mode)
{
    s_repeat = mode >= 0 && mode < REPEAT_COUNT ? mode : REPEAT_OFF;
    s_saved.repeat = s_repeat;
    save_state(0);
    bump();
}

int music_repeat(void) { return s_repeat; }

int music_peek(int ahead, const char **title, const char **artist)
{
    int pos = s_pos;
    if (pos < 0 || !s_list || !s_order)
        return -1;
    for (int i = 0; i < ahead && pos >= 0; i++)
        pos = order_next(pos, s_count, s_repeat, 1);
    if (pos < 0 || pos >= s_count)
        return -1;
    int t = s_order[pos];
    if (title) *title = s_list[t].title;
    if (artist) *artist = s_list[t].artist;
    return 0;
}

void music_toggle(void) { post(CMD_TOGGLE, 0); }
void music_next(void) { post(CMD_NEXT, 0); }
void music_prev(void) { post(CMD_PREV, 0); }
void music_seek(int delta_ms) { post(CMD_SEEK, delta_ms); }
void music_stop(void) { post(CMD_STOP, 0); }

void music_shutdown(void)
{
    if (!s_ready)
        return;
    if (s_pos >= 0) { /* where it was, written now: the stick is about to go to a game or the PC */
        note_place();
        save_state(1);
    }
    video_preview_stop(1); /* unloading AVCODEC would pull it from under a preview */
    int seq = post(CMD_QUIT, 0);
    wait_done(seq);
    SceUInt timeout = 1000000;
    sceKernelWaitThreadEnd(s_thread, &timeout);
    sceKernelDeleteThread(s_thread);
    s_thread = -1;
    sceMp3TermResource();
    if (s_loaded_mp3) sceUtilityUnloadModule(PSP_MODULE_AV_MP3);
    if (s_loaded_avcodec) sceUtilityUnloadModule(PSP_MODULE_AV_AVCODEC);
    s_loaded_mp3 = s_loaded_avcodec = 0;
    s_ready = 0;
    free(s_list);
    free(s_order);
    s_list = NULL;
    s_order = NULL;
    s_count = 0;
    s_state = MUSIC_STOPPED;
    s_current = -1;
    bump();
    power_hold(0);
}

int music_state(void) { return s_state; }
int music_generation(void) { return s_gen; }

int music_now(MusicNow *o)
{
    int i = s_current, pos = s_pos;
    if (i < 0 || i >= s_count || !s_list)
        return -1;
    o->title = s_list[i].title;
    o->artist = s_list[i].artist;
    o->path = s_list[i].path;
    o->index = pos >= 0 ? pos : i;
    o->count = s_count;
    o->elapsed_ms = s_elapsed_ms;
    o->duration_ms = s_duration_ms;
    return 0;
}
