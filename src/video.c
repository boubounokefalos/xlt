/* XLT - video player.
 *
 * MP4 (H.264 + AAC), using the firmware's own decoders the way cooleyes'
 * PMPlayer Advance does on 6.60 (layouts and call order taken from it as
 * facts; this is XLT's own code):
 *   - sceUtilityLoadAvModule(AVCODEC), then kernel-load flash0:/kd/mpeg_vsh.prx
 *     (the XMB's sceMpeg, which has sceMpegGetAvcNalAu);
 *   - sceMeBootStart660 (sceMeCore_driver 0x5DFF5C50) with type 4 for Baseline,
 *     3 for Main/High, 1 for > 480x272;
 *   - sceMpegCreate(mode 4, or 5 above 480x272) with a 2 MB "DDR top" area
 *     aligned to 4 MB; the access unit lives at DDR top + 0x10000;
 *   - per sample: sceMpegGetAvcNalAu (SPS/PPS + NAL), sceMpegAvcDecode,
 *     sceMpegAvcDecodeDetail2, then sceMpegBaseCscAvc to RGBA 8888 rows of
 *     512 (or 768) pixels;
 *   - AAC through sceAudiocodec (type 0x1003): workspace size in word 4, its
 *     pointer in word 3, sample rate in word 10; per frame src/size/dst/0x1000
 *     in words 6..9, giving 1024 stereo samples.
 * PMF uses psp5's sceMpeg ringbuffer path (tested on hardware), video only.
 *
 * Every step is written to <app>/cache/video.log (closed after each line,
 * so it survives a freeze), to make fixing this on real hardware possible.
 */
/* Parts of this file (the PMF (sceMpeg ringbuffer) path) are adapted from psp5 by 08Weber,
 * https://github.com/08Weber/psp5, GPL-3.0. */
#include <pspkernel.h>
#include <pspaudio.h>
#include <pspaudiocodec.h>
#include <pspgu.h>
#include <pspmpeg.h>
#include <psppower.h>
#include <psputility.h>
#include <psputility_avmodules.h>
#include <psputility_modules.h>
#include <kubridge.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "video.h"
#include "ui_internal.h"
#include "anim.h"
#include "mp4.h"
#include "music.h"
#include "sysctl.h"
#include "system.h"
#include "library.h"
#include "thumbconv.h"
#include "vplace.h"
#include "srt.h"
#include "bg.h"
#include "readwin.h"
#include "vqueue.h"
#include "gamecfg.h"
#include "vnext.h"
#include "h264strip.h"
#include "safestart.h"

#define ALREADY_LOADED 0x80020139
#define ALREADY_LOADED2 0x80111102
#define AV_ALREADY_LOADED 0x80110F02 /* sceUtilityLoadAvModule: SCE_ERROR_AV_MODULE_ALREADY_LOADED */
#define OVERLAY_FRAMES 180
#define FRAMES_MIN 3 /* one on screen, one queued, one being converted */
#define FRAMES_AHEAD_SMALL 8 /* full-screen playback decodes ahead: 480x272 pictures */
#define FRAMES_AHEAD_BIG 5   /* 720x480 pictures: about 1.5 MB each */
#define READ_WINDOW (256 * 1024)
#define MAX_W 720
#define MAX_H 480

/* ---------------------------------------------------- firmware structures */

typedef struct {
    void *sps; int sps_size;
    void *pps; int pps_size;
    int nal_prefix_size;
    void *nal; int nal_size;
    int mode;
} AvcNal;
typedef struct { int u0, u1, width, height, u4, u5, u6, u7, u8, u9; } AvcInfo;
typedef struct { void *b[8]; int u0, u1, u2; } AvcYuv;
typedef struct {
    int u0, u1, u2, u3;
    AvcInfo *info;
    int u5, u6, u7, u8, u9, u10;
    AvcYuv *yuv;
    int u12[12];
} AvcDetail2;
typedef struct { int height, width, mode0, mode1; void *b[8]; } AvcCsc;

int sceMpegGetAvcNalAu(SceMpeg *mpeg, AvcNal *nal, SceMpegAu *au);
int sceMpegAvcDecodeDetail2(SceMpeg *mpeg, AvcDetail2 **detail);
int sceMpegBaseCscAvc(void *dst, int unknown, int width, AvcCsc *csc);

/* ------------------------------------------------------------------ state */

enum { ST_IDLE, ST_LOADING, ST_PLAYING, ST_ENDED, ST_FAILED };
#define SUB_MAX (1 + MP4_SUB_MAX)
#define SUB_SIDE 100 /* sub_id of the .srt file */

static struct {
    int open;
    Spring t;
    char path[200], title[128];
    int pmf;
    volatile int state, stop, paused;
    char err[96];
    char why[96];   /* the decoder refused it: what the PSP has against it ("" if unknown) */
    /* frames: RGBA 8888 buffers of tbw x (fh rounded up) */
    u32 *buf[VQ_MAX_SLOTS];
    VQueue q;               /* full-screen MP4: pictures waiting for their time */
    int use_queue;
    volatile int clock_gen; /* the seek generation the clock has been started for */
    volatile int seek_from_ms; /* a forward seek: where it was (-1 = any other seek) */
    int tbw, fw, fh;
    volatile int published; /* buffer the decoder finished last, -1 none */
    int shown;              /* buffer the UI draws */
    volatile int pos_ms, dur_ms;
    volatile int seek_ms, seek_gen;
    volatile int audio_ms, audio_running, audio_gen_done, audio_ended;
    volatile u32 audio_t;   /* when audio_ms was last set (system time, us) */
    volatile int audio_step_ms; /* how long audio_ms stands still between updates */
    volatile int audio_req, audio_base_ms; /* the video is ready: start the sound here */
    int overlay;            /* frames left */
    SceUID vthread, athread;
    int last_state, last_paused, last_sec;
    /* preview: muted, in a box, looping; the thread exits by itself when told to stop */
    int preview;            /* 1 live preview, 2 grab one picture for a thumbnail */
    volatile int grabbed;   /* grab: the picture is in buf[published] */
    volatile int thread_done;
    int pv_start_ms;
    /* carrying on, and subtitles (full-screen playback only) */
    int resumed_ms;         /* started where it was stopped last time (△ goes to the start), 0 = no */
    int saved_ms;           /* the position last written to videopos.txt */
    /* subtitles: the .srt next to the video (if any) first, then the MP4's text tracks */
    Srt sub[SUB_MAX];
    volatile int nsub;      /* sources ready: the player thread adds the MP4's after opening it */
    volatile int sub_sel;   /* the one showing; -1 off, -2 not chosen yet */
    int sub_side;           /* sub[0] is the .srt file */
    int sub_id[SUB_MAX];    /* what each one is: SUB_SIDE, or the MP4's track number */
    int sub_want;           /* reopening: the one it had (an id), -1 off, -2 none kept */
    SceUID sthread;         /* reading the MP4's tracks, after the picture has started */
    char sub_label[SUB_MAX][48];
    char sub_lang[SUB_MAX][4];
    /* audio tracks (MP4): which one to open (-1: the preferred language, else the file's default) */
    int audio_track, naudio, audio_sel;
    char audio_label[MP4_AUDIO_MAX][48]; /* "Japanese · Director's commentary" */
    char audio_lang[MP4_AUDIO_MAX][4];   /* "jpn" */
} V = {.vthread = -1, .athread = -1, .sthread = -1, .published = -1, .shown = -1, .seek_from_ms = -1};

/* A PMF in memory (a game's ICON1.PMF), owned by the preview. */
static uint8_t *s_pmf_mem;
static u32 s_pmf_len;

static char s_log_path[220];

/* ------------------------------------------------- where videos were stopped */

static void vlog(const char *fmt, ...);

static VPlaces s_vp;
static char s_vp_path[220];

typedef struct {
    char *text;
    int len;
    char path[220];
} VpArg;

static void write_all(const char *path, const char *text, int len)
{
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return;
    sceIoWrite(fd, text, len);
    sceIoClose(fd);
}

static void job_vp(void *arg)
{
    const VpArg *a = arg;
    write_all(a->path, a->text, a->len);
    free(a->text);
}

/* Writes videopos.txt: on the worker, or now (sync) before the stick goes away. */
static void vp_save(int sync)
{
    int cap = VP_MAX * 230 + 1;
    VpArg a;
    a.text = malloc(cap);
    if (!a.text)
        return;
    a.len = vp_write(&s_vp, a.text, cap);
    snprintf(a.path, sizeof(a.path), "%s", s_vp_path);
    if (a.len < 0 || sync) {
        if (a.len >= 0) write_all(a.path, a.text, a.len);
        free(a.text);
        return;
    }
    bg_post(job_vp, &a, sizeof(a)); /* the worker writes, then frees the text */
}

/* Remembers where the open video is (only once it has played). */
static void remember_place(int sync)
{
    if (!V.open || V.preview || V.pmf || V.state != ST_PLAYING)
        return;
    vp_set(&s_vp, V.path, V.pos_ms, V.dur_ms);
    V.saved_ms = V.pos_ms;
    vp_save(sync);
}

int video_place(const char *path, int *dur_ms) { return vp_get(&s_vp, path, dur_ms); }

static const Srt *subs_now(void)
{
    int k = V.sub_sel;
    return k >= 0 && k < V.nsub ? &V.sub[k] : NULL;
}

/* Only with the player thread stopped: it adds to the list. */
static void subs_free(void)
{
    V.nsub = 0;
    V.sub_side = 0;
    for (int k = 0; k < SUB_MAX; k++)
        srt_free(&V.sub[k]);
}

/* What is showing, as an id to find it again after reopening: -1 off, -2 not chosen yet. */
static int subs_id_now(void)
{
    int k = V.sub_sel;
    return k >= 0 && k < V.nsub ? V.sub_id[k] : k == -1 ? -1 : -2;
}

/* The MP4 track to read first and show: the one it had, else the language
 * picked last ("off": none), else the file's default; -1 none. */
static int subs_first(const Mp4 *m, int want)
{
    if (want >= 0 && want < m->nsub) return want;
    if (want == -1 || !strcmp(g_cfg.sub_lang, "off")) return -1;
    for (int k = 0; k < m->nsub && g_cfg.sub_lang[0]; k++)
        if (!strcmp(m->subs[k].lang, g_cfg.sub_lang)) return k;
    for (int k = 0; k < m->nsub; k++)
        if (m->subs[k].enabled) return k;
    return -1;
}

static Mp4SubInfo s_sinfo[MP4_SUB_MAX];
static int s_snsub, s_sfirst;

/* One MP4 track read (on the subtitle thread): it joins the list after the others. */
static void subs_done(int k, char *text, int len, void *ctx)
{
    (void)ctx;
    int at = V.nsub, cues = 0;
    if (at < SUB_MAX && text && (cues = srt_parse(&V.sub[at], text, len)) > 0) {
        const Mp4SubInfo *si = &s_sinfo[k];
        memcpy(V.sub_lang[at], si->lang, sizeof(V.sub_lang[at]));
        const char *l = si->lang[0] ? mp4_lang_name(si->lang) : "";
        if (l[0] && si->name[0]) snprintf(V.sub_label[at], sizeof(V.sub_label[at]), "%s" DOT "%.30s", l, si->name);
        else if (l[0] || si->name[0]) snprintf(V.sub_label[at], sizeof(V.sub_label[at]), "%.40s", l[0] ? l : si->name);
        else snprintf(V.sub_label[at], sizeof(V.sub_label[at]), "Track %d", k + 1);
        V.sub_id[at] = k;
        V.nsub = at + 1; /* the UI may use it from now on */
        if (k == s_sfirst && V.sub_sel == -2)
            V.sub_sel = at;
    } else if (at < SUB_MAX) {
        srt_free(&V.sub[at]);
    }
    if (k == s_sfirst && V.sub_sel == -2) /* the one wanted had nothing */
        V.sub_sel = -1;
    free(text);
    vlog("subtitle track %d: %s, %d lines%s", k + 1, s_sinfo[k].lang[0] ? s_sinfo[k].lang : "und", cues,
         s_sinfo[k].enabled ? ", default" : "");
}

static int subs_thread(SceSize args, void *argp)
{
    u32 t0 = sceKernelGetSystemTimeLow();
    int order[MP4_SUB_MAX], n = 0;
    if (s_sfirst >= 0) order[n++] = s_sfirst;
    for (int k = 0; k < s_snsub; k++)
        if (k != s_sfirst) order[n++] = k;
    mp4_sub_read(V.path, order, n, subs_done, NULL, &V.stop);
    if (V.sub_sel == -2)
        V.sub_sel = -1;
    vlog("subtitle tracks read in %u ms%s", (sceKernelGetSystemTimeLow() - t0) / 1000, V.stop ? " (stopped)" : "");
    return 0;
}

/* The MP4's text tracks are read on their own low thread, so the picture never
 * waits for them (a film's take seconds: one Memory Stick read per line). */
static void start_subs(const Mp4 *m)
{
    s_snsub = m->nsub;
    memcpy(s_sinfo, m->subs, sizeof(s_sinfo));
    s_sfirst = subs_first(m, V.sub_want);
    if (V.sub_sel == -2 && s_sfirst < 0)
        V.sub_sel = -1;
    V.sthread = sceKernelCreateThread("xl_subs", subs_thread, 0x30, 0x8000, PSP_THREAD_ATTR_USER, NULL);
    if (V.sthread >= 0 && sceKernelStartThread(V.sthread, 0, NULL) < 0) {
        sceKernelDeleteThread(V.sthread);
        V.sthread = -1;
    }
    if (V.sthread < 0 && V.sub_sel == -2)
        V.sub_sel = -1;
}

/* The .srt with the video's name, next to it: sub[0]. */
static void load_subs(void)
{
    subs_free();
    char p[220];
    const char *dot = strrchr(V.path, '.');
    snprintf(p, sizeof(p), "%.*s.srt", dot ? (int)(dot - V.path) : (int)strlen(V.path), V.path);
    SceUID fd = sceIoOpen(p, PSP_O_RDONLY, 0);
    if (fd < 0)
        return;
    int size = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    char *b = size > 0 && size < 2 * 1024 * 1024 ? malloc(size) : NULL;
    int n = b ? sceIoRead(fd, b, size) : -1;
    sceIoClose(fd);
    if (n > 0 && srt_parse(&V.sub[0], b, n) > 0) {
        V.sub_side = 1;
        V.sub_id[0] = SUB_SIDE;
        V.sub_lang[0][0] = 0;
        snprintf(V.sub_label[0], sizeof(V.sub_label[0]), "Subtitle file");
        V.nsub = 1;
    } else {
        srt_free(&V.sub[0]);
    }
    free(b);
    vlog("subtitles: %s, %d lines", p, V.sub[0].n);
}
static int s_vsh_mod = -1, s_av_loaded;
static int s_keep_sub = -2; /* reopening the same video (after sleep): the subtitles it had */

static void vlog_ap(int force, const char *fmt, va_list ap)
{
    if (V.preview && !force)
        return; /* previews come and go with the focus: only their start and failures are logged */
    char line[200];
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    SceUID fd = sceIoOpen(s_log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, line, n);
        sceIoClose(fd);
    }
}

static void vlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_ap(0, fmt, ap);
    va_end(ap);
}

static void vlog_always(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_ap(1, fmt, ap);
    va_end(ap);
}

void video_note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_ap(1, fmt, ap);
    va_end(ap);
}

static int s_pv_seen; /* the running preview has shown a picture (logged once) */

static void fail(const char *why, int code)
{
    snprintf(V.err, sizeof(V.err), "%s", why);
    vlog_always("%sFAILED: %s (%08X)", V.preview ? "preview " : "", why, code);
    V.state = ST_FAILED;
}

/* The clock for decoding: only the full-screen player raises it. Previews and
 * still-frame grabs leave it alone, because every clock change makes the whole
 * screen jump and browsing starts and stops them all the time. */
static int s_held;
static void hold_on(void)
{
    s_held = !V.preview;
    if (s_held) power_hold(333);
}
static void hold_off(void)
{
    if (s_held) power_hold(0);
    s_held = 0;
}

static int already(int r)
{
    return r >= 0 || (u32)r == ALREADY_LOADED || (u32)r == ALREADY_LOADED2 || (u32)r == AV_ALREADY_LOADED;
}

/* --------------------------------------------------------------- frames */

static int alloc_frames(int tbw, int h, int want)
{
    int rows = ((h + 15) & ~15) + 1; /* CSC writes whole macroblock rows; +1 for filtering */
    size_t size = ((size_t)tbw * rows * 4 + 63) & ~(size_t)63;
    int n = 0;
    for (; n < want && n < VQ_MAX_SLOTS; n++) {
        V.buf[n] = memalign(64, size);
        if (!V.buf[n]) break;
        memset(V.buf[n], 0, size);
        /* the decoder writes past the CPU cache: flush it once, then leave it alone */
        sceKernelDcacheWritebackInvalidateRange(V.buf[n], size);
    }
    if (n < want && n > FRAMES_MIN) { /* memory ran out: leave some for the decoder */
        free(V.buf[--n]);
        V.buf[n] = NULL;
    }
    if (n < FRAMES_MIN)
        return -1;
    vq_reset(&V.q, n);
    V.tbw = tbw;
    return 0;
}

static void free_frames(void)
{
    for (int i = 0; i < VQ_MAX_SLOTS; i++) {
        free(V.buf[i]);
        V.buf[i] = NULL;
    }
    vq_reset(&V.q, 0);
    V.use_queue = 0;
    V.published = V.shown = -1;
}

/* A buffer that is neither on screen nor queued for it; -1 when stopping.
 * With three buffers there always is one, so the decoder never waits for
 * the screen (with two it did, and missed every other picture's slot). */
static int free_slot(void)
{
    for (;;) {
        int k = vq_free_slot(&V.q, V.shown, V.published);
        if (k >= 0) return k;
        if (V.stop) return -1;
        sceKernelDelayThread(2000);
    }
}

/* ----------------------------------------------------------------- clock */

static u64 s_clock_us;   /* system time at s_clock_ms */
static int s_clock_ms;

static void clock_reset(int ms)
{
    s_clock_ms = ms;
    s_clock_us = sceKernelGetSystemTimeWide();
}

/* Playback time: the audio's when it plays, otherwise wall time. */
static int clock_now(void)
{
    if (V.audio_running && V.audio_gen_done == V.seek_gen && !V.audio_ended) {
        int ms = V.audio_ms;
        if (!V.paused) { /* audio_ms moves in 21 ms steps: fill in between them */
            u32 d = (sceKernelGetSystemTimeLow() - V.audio_t) / 1000;
            ms += d > (u32)V.audio_step_ms ? V.audio_step_ms : (int)d;
        }
        return ms;
    }
    return s_clock_ms + (int)((sceKernelGetSystemTimeWide() - s_clock_us) / 1000);
}

/* Sleeps until the clock reaches ms (paused time doesn't count). */
static void wait_until(int ms)
{
    while (!V.stop) {
        if (V.paused) {
            int keep = clock_now();
            while (V.paused && !V.stop)
                sceKernelDelayThread(10000);
            clock_reset(keep);
            continue;
        }
        int d = ms - clock_now();
        if (d <= 0 || d > 2000) /* far ahead: don't stall on a bad timestamp */
            return;
        sceKernelDelayThread(d > 10 ? 10000 : d * 1000);
    }
}

/* -------------------------------------------------------------- modules */

static void me_boot(int boot_type)
{
    static const char *const me_mods[] = {"sceMeCodecWrapper", "sceMeCore", "sceMeCore_driver", NULL};
    int res = 0;
    const char *which = "";
    if (sysctl_kcall_nid(me_mods, "sceMeCore_driver", 0x5DFF5C50, boot_type, &res, &which) == 0)
        vlog("sceMeBootStart660(%d) in %s = %08X", boot_type, which, res);
    else
        vlog("sceMeBootStart660 not found (continuing)");
}

static int load_mp4_modules(void)
{
    int r = sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC);
    vlog("sceUtilityLoadAvModule(AVCODEC) = %08X", r);
    if (!already(r)) return -1;
    s_av_loaded = 1;
    if (s_vsh_mod < 0) {
        SceUID id = kuKernelLoadModule("flash0:/kd/mpeg_vsh.prx", 0, NULL);
        vlog("kuKernelLoadModule(mpeg_vsh.prx) = %08X", id);
        if (id < 0 && (u32)id != ALREADY_LOADED) return -1;
        if (id >= 0) {
            int st = 0;
            r = sceKernelStartModule(id, 0, NULL, &st, NULL);
            vlog("sceKernelStartModule(mpeg_vsh) = %08X", r);
            if (r < 0) return -1;
        }
        s_vsh_mod = id >= 0 ? id : 0;
    }
    return 0;
}

/* ------------------------------------------------------- the AVC decoder */

typedef struct {
    SceMpeg mpeg;
    SceMpegRingbuffer rb; /* unused by the NAL path, but sceMpegCreate wants one */
    void *mbuf;
    SceMpegAu *au;
    int inited, created, mode;
} Dec;

/* 2 MB aligned to 4 MB, kept for the session: a memalign that big may not succeed twice. */
static void *s_ddr;

static void dec_close(Dec *d)
{
    if (d->created == 0)
        sceMpegDelete(&d->mpeg);
    if (d->inited == 0)
        sceMpegFinish();
    free(d->mbuf);
    free(d->au);
    memset(d, 0, sizeof(*d));
    d->inited = d->created = -1;
}

/* mode 4 (up to 480x272) or 5 (up to 720x480, more frame memory); boot = ME boot type. */
static int dec_open(Dec *d, int mode, int boot)
{
    memset(d, 0, sizeof(*d));
    d->inited = d->created = -1;
    d->mode = mode;
    me_boot(boot);
    d->inited = sceMpegInit();
    vlog("sceMpegInit = %08X", d->inited);
    int msize = sceMpegQueryMemSize(mode);
    vlog("sceMpegQueryMemSize(%d) = %d", mode, msize);
    if (!s_ddr)
        s_ddr = memalign(0x400000, 0x200000);
    d->mbuf = msize > 0 ? memalign(64, msize) : NULL;
    d->au = memalign(64, 64);
    if (!s_ddr || !d->mbuf || !d->au) {
        fail("not enough memory for the decoder", 0);
        return -1;
    }
    d->created = sceMpegCreate(&d->mpeg, d->mbuf, msize, &d->rb, 512, mode, (SceInt32)s_ddr);
    vlog("sceMpegCreate(mode %d) = %08X (ddr %p)", mode, d->created, s_ddr);
    if (d->created != 0) {
        fail("sceMpegCreate failed", d->created);
        return -1;
    }
    memset(d->au, 0xFF, 64);
    int r = sceMpegInitAu(&d->mpeg, (uint8_t *)s_ddr + 0x10000, d->au);
    vlog("sceMpegInitAu = %08X", r);
    if (r != 0) {
        fail("sceMpegInitAu failed", r);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------- AAC audio */

static struct {
    Mp4 *m;
    SceUID fd;
} A;

#define PCM_BUFS 4          /* the hardware plays one while the next is decoded */
#define ACHUNK_MAX (192 * 1024)

/* Audio is read a whole MP4 chunk (usually about a second) at a time, so one slow
 * Memory Stick read can't make the output miss its 23 ms deadline. */
static int audio_fill(const Mp4Track *t, uint32_t j, uint8_t *buf, uint32_t *first, uint32_t *count)
{
    uint32_t c0, cn;
    mp4_chunk_of(t, j, &c0, &cn);
    uint64_t off = mp4_offset(t, j);
    uint32_t len = 0, n = 0;
    for (uint32_t s = j; s < c0 + cn && len + mp4_size(t, s) <= ACHUNK_MAX; s++, n++)
        len += mp4_size(t, s);
    if (n == 0 || sceIoLseek(A.fd, off, PSP_SEEK_SET) < 0 || sceIoRead(A.fd, buf, len) != (int)len)
        return -1;
    sceKernelDcacheWritebackRange(buf, len); /* the decoder reads RAM directly */
    *first = j;
    *count = n;
    return 0;
}

static int audio_thread(SceSize args, void *argp)
{
    static unsigned long codec[65] __attribute__((aligned(64)));
    static short pcm[PCM_BUFS][1024 * 2] __attribute__((aligned(64)));
    const Mp4 *m = A.m;
    const Mp4Track *t = &m->audio;
    /* The CPU never touches these through the cache: no dirty line can later be
     * written back over sound the decoder put there. */
    sceKernelDcacheWritebackInvalidateRange(pcm, sizeof(pcm));
    memset(codec, 0, sizeof(codec));
    int r = sceAudiocodecCheckNeedMem(codec, PSP_CODEC_AAC);
    vlog("audio: CheckNeedMem = %08X, %lu bytes", r, codec[4]);
    void *work = r >= 0 && codec[4] ? memalign(64, codec[4]) : NULL;
    if (!work) return 0;
    codec[3] = (unsigned long)work;
    codec[10] = (unsigned long)m->sample_rate;
    r = sceAudiocodecInit(codec, PSP_CODEC_AAC);
    vlog("audio: Init(%d Hz) = %08X", m->sample_rate, r);
    if (r < 0 || sceAudioSRCChReserve(1024, m->sample_rate, 2) < 0) {
        vlog("audio: no SRC channel / init failed");
        free(work);
        return 0;
    }
    uint8_t *chunk = memalign(64, ACHUNK_MAX);
    uint32_t c_first = 0, c_count = 0, c_pos = 0; /* samples in `chunk`, and the byte position of the next one */
    int gen = -1, played = 0, base_ms = 0, errors = 0, slot = 0;
    uint32_t j = 0;
    V.audio_step_ms = 1024 * 1000 / m->sample_rate + 1;
    V.audio_running = 1;
    while (!V.stop && chunk) {
        if (V.audio_req != V.seek_gen) { /* a seek: silent until the video has found its key frame */
            sceKernelDelayThread(2000);
            continue;
        }
        if (gen != V.audio_req) { /* (re)start at the video's position */
            gen = V.audio_req;
            j = mp4_sample_at(t, V.audio_base_ms);
            base_ms = mp4_time_ms(t, j);
            played = 0;
            c_count = 0;
            V.audio_ms = base_ms;
            V.audio_t = sceKernelGetSystemTimeLow();
            V.audio_ended = 0;
            V.audio_gen_done = gen;
        }
        if (V.paused) {
            sceKernelDelayThread(10000);
            continue;
        }
        if (j >= t->count) {
            if (!V.audio_ended) { /* the video may still be playing: on the wall clock now */
                clock_reset(V.audio_ms);
                V.audio_ended = 1;
            }
            sceKernelDelayThread(10000);
            continue;
        }
        if (j < c_first || j >= c_first + c_count) {
            if (audio_fill(t, j, chunk, &c_first, &c_count) != 0) {
                if (errors++ < 5) vlog("audio: read error at sample %u", j);
                j++;
                continue;
            }
            c_pos = 0;
        }
        uint32_t size = mp4_size(t, j);
        short *out = pcm[slot];
        codec[6] = (unsigned long)(chunk + c_pos);
        codec[7] = size;
        codec[8] = (unsigned long)out;
        codec[9] = sizeof(pcm[0]);
        r = sceAudiocodecDecode(codec, PSP_CODEC_AAC);
        if (r < 0) {
            if (errors++ < 5) vlog("audio: Decode(sample %u) = %08X", j, r);
            memset((void *)((u32)out | 0x40000000u), 0, sizeof(pcm[0])); /* silence, uncached */
        }
        sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, out);
        slot = (slot + 1) % PCM_BUFS;
        played += 1024;
        V.audio_ms = base_ms + (int)((long long)played * 1000 / m->sample_rate);
        V.audio_t = sceKernelGetSystemTimeLow();
        c_pos += size;
        j++;
    }
    V.audio_running = 0;
    for (int i = 0; i < 100 && sceAudioSRCChRelease() < 0; i++)
        sceKernelDelayThread(2000);
    free(chunk);
    free(work);
    return 0;
}

/* ------------------------------------------------------------- MP4 video */

static int file_read_at(void *ctx, uint64_t off, void *dst, uint32_t len)
{
    SceUID fd = *(SceUID *)ctx;
    if (sceIoLseek(fd, off, PSP_SEEK_SET) < 0)
        return -1;
    return sceIoRead(fd, dst, len);
}

/* The UI thread's end of the queue. */
static int s_pause_held, s_pause_keep, s_ui_gen, s_ui_shown, s_ui_starved, s_ui_waiting;
static u32 s_ui_last, s_ui_gap_max;
static u32 s_tick_last, s_tick_max, s_tick_sum, s_tick_n, s_ge_max, s_ge_sum;

static void ui_stats_reset(void)
{
    s_pause_held = s_ui_shown = s_ui_starved = s_ui_waiting = 0;
    s_ui_gen = -1;
    s_ui_last = s_ui_gap_max = 0;
    s_tick_last = s_tick_max = s_tick_sum = s_tick_n = s_ge_max = s_ge_sum = 0;
}

static int queue_take(void)
{
    u32 tick = sceKernelGetSystemTimeLow(), ge = gfx_ge_wait_us();
    if (s_tick_last && !V.paused) { /* how the screen loop itself is doing */
        u32 d = tick - s_tick_last;
        if (d > s_tick_max) s_tick_max = d;
        if (ge > s_ge_max) s_ge_max = ge;
        s_tick_sum += d;
        s_ge_sum += ge;
        s_tick_n++;
    }
    s_tick_last = tick;
    if (V.paused) {
        if (!s_pause_held) {
            s_pause_keep = clock_now();
            s_pause_held = 1;
        }
        s_ui_last = 0;
    } else if (s_pause_held) {
        clock_reset(s_pause_keep); /* paused time doesn't count */
        s_pause_held = 0;
    }
    int ms = 0, gen = V.seek_gen;
    int slot = vq_take(&V.q, gen, V.clock_gen, clock_now(), V.paused, &ms);
    if (slot < 0) {
        if (vq_empty(&V.q) && V.clock_gen == gen && !V.paused && s_ui_gen == gen && !s_ui_waiting) {
            s_ui_starved++; /* time passed with no picture ready */
            s_ui_waiting = 1;
        }
        return 0;
    }
    V.shown = slot; /* first: then the decoder can't pick this slot */
    vq_release(&V.q, slot);
    V.pos_ms = ms;
    s_ui_waiting = 0;
    s_ui_shown++;
    u32 now = sceKernelGetSystemTimeLow();
    if (s_ui_gen == gen && s_ui_last && now - s_ui_last > s_ui_gap_max)
        s_ui_gap_max = now - s_ui_last;
    s_ui_gen = gen;
    s_ui_last = now;
    return 1;
}

/* The first pictures are ready: the clock and the sound start from them. */
static void start_playing(int gen, int base_ms)
{
    clock_reset(base_ms);
    s_pause_keep = base_ms; /* a seek made while paused resumes from here */
    V.audio_base_ms = base_ms;
    V.audio_req = gen;
    V.clock_gen = gen;
}

static int mp4_thread(SceSize args, void *argp)
{
    static Mp4 m;
    const char *why = "";
    hold_on(); /* the whole playback: a clock change mid-video halves the frame rate */
    if (mp4_open_track(&m, V.path, &why, V.audio_track, g_cfg.audio_lang) != 0) {
        fail(why, 0);
        hold_off();
        return 0;
    }
    if (!V.preview) {
        V.naudio = m.naudio;
        V.audio_sel = m.audio_sel;
        for (int k = 0; k < m.naudio; k++) {
            const Mp4AudioInfo *a = &m.audios[k];
            memcpy(V.audio_lang[k], a->lang, sizeof(V.audio_lang[k]));
            const char *l = a->lang[0] ? mp4_lang_name(a->lang) : "";
            if (l[0] && a->name[0]) snprintf(V.audio_label[k], sizeof(V.audio_label[k]), "%s" DOT "%.30s", l, a->name);
            else if (l[0] || a->name[0]) snprintf(V.audio_label[k], sizeof(V.audio_label[k]), "%.40s", l[0] ? l : a->name);
            else snprintf(V.audio_label[k], sizeof(V.audio_label[k]), "Track %d", k + 1);
            vlog("audio track %d: %s, %d Hz, %d ch, AAC object %d%s%s", k + 1, V.audio_label[k], a->sample_rate,
                 a->channels, a->aac_object, a->enabled ? ", default" : "", k == m.audio_sel ? " <- playing" : "");
        }
        if (m.nsub)
            start_subs(&m);
        else if (V.sub_sel == -2)
            V.sub_sel = -1;
    }
    if (V.preview) /* past the logos and black frames at the start (a grab too) */
        V.seek_ms = V.pv_start_ms = m.duration_ms > 20000 ? m.duration_ms / 10 : 0;
    vlog("mp4: %dx%d profile %d level %d, nal %d, %u frames, %d ms; audio %s %d Hz %d ch obj %d",
         m.width, m.height, m.profile, m.level, m.nal_length, m.video.count, m.duration_ms,
         m.audio.present ? "yes" : "no", m.sample_rate, m.channels, m.aac_object);
    V.dur_ms = m.duration_ms;
    if (m.width <= 0 || m.height <= 0 || m.width > MAX_W || m.height > MAX_H) {
        fail("video larger than 720x480", 0);
        mp4_close(&m);
        hold_off();
        return 0;
    }
    int big = m.width > 480 || m.height > 272;
    int refs = h264_sps_refs(m.sps, m.sps_size), wp = -1, wb = -1;
    h264_pps_weights(m.pps, m.pps_size, &wp, &wb);
    /* Weighted prediction (x264's default) is refused by the PSP: the weights are taken
     * out of the PPS and every slice as they're read (h264strip.c), as PMPlayer does. */
    static H264Strip strip;
    static uint8_t strip_pps[512];
    int strip_pps_n = h264_strip_init(&strip, m.sps, m.sps_size, m.pps, m.pps_size) == 1
                          ? h264_strip_pps(m.pps, m.pps_size, strip_pps, sizeof(strip_pps)) : -1;
    int stripping = strip_pps_n > 0, strip_failed = 0;
    if (wp == 1 || wb == 1)
        vlog("mp4: weighted prediction: %s", stripping ? "taking the weights out" : "can't read the headers to take it out");
    /* Streams beyond what PSP encodes use (more than 3 reference frames, or
     * weighted P prediction left in) start in mode 5, as PMPlayer's compatibility path does. */
    int compat = refs > 3 || (wp == 1 && !stripping);
    int mode = big || compat ? 5 : 4;
    int boot = mode == 5 ? 1 : m.profile == 66 ? 4 : 3;
    vlog("mp4: refs %d, weighted_pred %d, weighted_bipred %d -> mode %d", refs, wp, wb, mode);
    SceUID fd = sceIoOpen(V.path, PSP_O_RDONLY, 0);
    if (fd < 0 || load_mp4_modules() != 0) {
        fail(fd < 0 ? "can't open the file" : "couldn't load the video decoder", 0);
        if (fd >= 0) sceIoClose(fd);
        mp4_close(&m);
        hold_off();
        return 0;
    }
    static Dec dec;
    uint32_t sbuf_cap = 256 * 1024;
    uint8_t *sbuf = memalign(64, sbuf_cap);
    uint32_t xbuf_cap = 0;
    uint8_t *xbuf = NULL; /* a sample without its weights */
    int frames_ok = 0, dropped = 0, r = 0, retried = 0;
    AvcDetail2 *d2 = NULL;
    dec.inited = dec.created = -1;
    /* Full-screen playback reads ahead and decodes ahead; a preview needs neither. */
    uint8_t *window = V.preview ? NULL : memalign(64, READ_WINDOW);
    ReadWin rw;
    readwin_init(&rw, window, READ_WINDOW, file_read_at, &fd);
    if (!sbuf || alloc_frames(big ? 768 : 512, m.height,
                              V.preview ? FRAMES_MIN : big ? FRAMES_AHEAD_BIG : FRAMES_AHEAD_SMALL) != 0) {
        fail("not enough memory for the decoder", 0);
        goto done;
    }
    V.audio_req = V.clock_gen = -1;
    V.audio_ended = 0;
    ui_stats_reset();
    V.use_queue = !V.preview;
    u32 read_max = 0, decode_max = 0, csc_max = 0, csc_sum = 0, csc_n = 0;
    int prebuffer = V.q.nslots - 2 > 3 ? 3 : V.q.nslots - 2; /* leaves a slot free to fill */
    if (dec_open(&dec, mode, boot) != 0)
        goto done;

    V.fw = m.width;
    V.fh = m.height;
    if (V.preview) {
        /* muted */
    } else if (m.audio.present && m.aac_object == 2) {
        A.m = &m;
        A.fd = sceIoOpen(V.path, PSP_O_RDONLY, 0);
        V.athread = sceKernelCreateThread("xl_vaudio", audio_thread, 0x12, 0x10000, PSP_THREAD_ATTR_USER, NULL);
        if (V.athread >= 0) sceKernelStartThread(V.athread, 0, NULL);
    } else if (m.audio.present) {
        vlog("audio: AAC object %d isn't supported (only AAC LC): playing without sound", m.aac_object);
    }

    /* the video track's own timing (the file's duration may be the audio's) */
    uint32_t track_ms = mp4_time_ms(&m.video, m.video.count);
    uint32_t frame_ms_x1000 = m.video.count && track_ms ? (uint32_t)((unsigned long long)track_ms * 1000 / m.video.count) : 33367;
    vlog("mp4: frame %u.%03u ms", frame_ms_x1000 / 1000, frame_ms_x1000 % 1000);
    int gen = -1, base_ms = 0, nout = 0, errors = 0, first_done = 0, started = 0, queued = 0;
    uint32_t i = 0;
    V.state = ST_PLAYING;
    while (!V.stop) {
        if (gen != V.seek_gen) {
            gen = V.seek_gen;
            i = mp4_sync_before(&m.video, mp4_sample_at(&m.video, V.seek_ms));
            if (V.seek_from_ms >= 0) { /* forward must not land behind where it was */
                if ((int)mp4_time_ms(&m.video, i) <= V.seek_from_ms) {
                    uint32_t next = mp4_sync_after(&m.video, i + 1);
                    if (next < m.video.count) i = next;
                }
                V.seek_from_ms = -1;
            }
            base_ms = mp4_time_ms(&m.video, i);
            V.seek_ms = base_ms; /* the audio starts from the same key frame */
            nout = 0;
            first_done = 0; /* the decoder needs mode 3 again from the key frame */
            queued = 0;
            started = V.preview; /* full-screen: the clock waits for the first pictures */
            if (V.preview) clock_reset(base_ms);
        }
        if (V.preview && (i >= m.video.count || V.pos_ms > V.pv_start_ms + 30000)) {
            V.seek_ms = V.pv_start_ms; /* a preview loops its first 30 seconds */
            V.seek_gen++;
            continue;
        }
        if (i >= m.video.count) {
            if (!V.use_queue || V.state != ST_PLAYING)
                break;
            if (!started) { /* a clip shorter than the pictures it waits for */
                start_playing(gen, base_ms);
                started = 1;
            }
            if (vq_empty(&V.q))
                break;
            sceKernelDelayThread(5000); /* the last pictures are still to be shown */
            continue;
        }
        uint32_t size = mp4_size(&m.video, i);
        if (size > sbuf_cap) {
            free(sbuf);
            sbuf_cap = (size + 0xFFFF) & ~0xFFFFu;
            sbuf = memalign(64, sbuf_cap);
            if (!sbuf) { fail("not enough memory for a frame", 0); break; }
        }
        u32 t0 = sceKernelGetSystemTimeLow();
        if (readwin_get(&rw, mp4_offset(&m.video, i), size, sbuf) != 0) {
            fail("read error", 0);
            break;
        }
        u32 t1 = sceKernelGetSystemTimeLow();
        if (t1 - t0 > read_max) read_max = t1 - t0;
        uint8_t *data = sbuf;
        uint32_t dsize = size;
        if (stripping) {
            uint32_t need = size + size / 4 + 64;
            if (need > xbuf_cap) {
                free(xbuf);
                xbuf_cap = (need + 0xFFFF) & ~0xFFFFu;
                xbuf = memalign(64, xbuf_cap);
                if (!xbuf) xbuf_cap = 0;
            }
            int k = xbuf ? h264_strip_sample(&strip, sbuf, size, m.nal_length, xbuf, xbuf_cap) : -1;
            if (k >= 0) {
                data = xbuf;
                dsize = (uint32_t)k;
            } else if (strip_failed++ < 3) {
                vlog("mp4: sample %u: couldn't take the weights out, sent as it is", i);
            }
        }
        sceKernelDcacheWritebackRange(data, dsize); /* the decoder reads RAM directly */
        /* As PMPlayer: mode 3 for the first packet and for IDR frames, 0 otherwise. */
        int mode = !first_done || h264_sample_has_idr(data, dsize, m.nal_length) ? 3 : 0;
        AvcNal nal = {m.sps, m.sps_size, stripping ? strip_pps : m.pps, stripping ? strip_pps_n : m.pps_size,
                      m.nal_length, data, (int)dsize, mode};
        SceInt32 pic = 0;
        r = sceMpegGetAvcNalAu(&dec.mpeg, &nal, dec.au);
        if (r == 0)
            r = sceMpegAvcDecode(&dec.mpeg, dec.au, 512, 0, &pic);
        if (r == 0)
            r = sceMpegAvcDecodeDetail2(&dec.mpeg, &d2);
        u32 t2 = sceKernelGetSystemTimeLow();
        if (t2 - t1 > decode_max) decode_max = t2 - t1;
        if (r != 0 && !frames_ok && !first_done && !retried && !(big && dec.mode == 5)) {
            /* the first key frame is refused: the other decoder mode may take it */
            int other = dec.mode == 5 ? 4 : 5;
            vlog("decode(sample %u) = %08X in mode %d: retrying in mode %d", i, r, dec.mode, other);
            retried = 1;
            dec_close(&dec);
            if (dec_open(&dec, other, other == 5 ? 1 : m.profile == 66 ? 4 : 3) != 0)
                break;
            continue; /* the same sample again */
        }
        if (r != 0) {
            if (errors++ < 5) vlog("decode(sample %u, %u bytes, mode %d) = %08X", i, size, mode, r);
            if (errors > 30 && !frames_ok) {
                char why[96];
                if (h264_psp_problem(&m, stripping, why, sizeof(why)))
                    snprintf(V.why, sizeof(V.why), "%s", why); /* shown with how to re-encode it */
                vlog("why: %s", V.why[0] ? V.why : "nothing known against it");
                fail("The PSP can't play this video", r);
                break;
            }
            i++;
            continue;
        }
        first_done = 1;
        if (pic > 4) pic = 4;
        /* One decode can finish several pictures (B-frames): show each, in order. */
        for (int k = 0; k < pic && d2 && d2->info && d2->yuv && !V.stop; k++) {
            const AvcInfo *info = d2->info + k;
            const AvcYuv *yuv = d2->yuv + k;
            if (frames_ok == 0)
                vlog("first picture: %dx%d", info->width, info->height);
            if (info->width <= 0 || info->width > MAX_W || info->height <= 0 || info->height > MAX_H)
                continue;
            int late_ms = base_ms + (int)((unsigned long long)nout * frame_ms_x1000 / 1000);
            if (started && !V.paused && clock_now() > late_ms + 2 * (int)(frame_ms_x1000 / 1000)) {
                /* already late: skip it (not even converted) so the picture catches up */
                nout++;
                dropped++;
                if (V.preview) V.pos_ms = late_ms;
                continue;
            }
            int next = free_slot();
            if (next < 0) break;
            AvcCsc csc;
            csc.height = (info->height + 15) >> 4;
            csc.width = (info->width + 15) >> 4;
            csc.mode0 = csc.mode1 = 0;
            memcpy(csc.b, yuv->b, sizeof(csc.b));
            u32 t3 = sceKernelGetSystemTimeLow();
            r = sceMpegBaseCscAvc(V.buf[next], 0, V.tbw, &csc);
            t3 = sceKernelGetSystemTimeLow() - t3;
            if (t3 > csc_max) csc_max = t3;
            csc_sum += t3;
            csc_n++;
            if (r != 0) {
                if (errors++ < 5) vlog("sceMpegBaseCscAvc = %08X", r);
                continue;
            }
            int out_ms = base_ms + (int)((unsigned long long)nout * frame_ms_x1000 / 1000);
            if (V.use_queue) {
                vq_push(&V.q, next, out_ms, gen);
                if (!started && ++queued >= prebuffer) {
                    start_playing(gen, base_ms);
                    started = 1;
                }
            } else {
                wait_until(out_ms);
                V.pos_ms = out_ms;
                V.published = next;
            }
            nout++;
            frames_ok++;
            if (V.preview == 2) {
                V.grabbed = 1;
                break;
            }
        }
        if (V.grabbed)
            break;
        i++;
    }
    if (!V.stop && V.state == ST_PLAYING)
        V.state = ST_ENDED;
    vlog("mp4: %s after %d pictures, %d dropped late (sample %u of %u, %d decode errors, mode %d); video %d ms, audio %d ms",
         V.stop ? "stopped" : V.state == ST_ENDED ? "ended" : "failed", frames_ok, dropped, i, m.video.count,
         errors, dec.mode, V.pos_ms, V.audio_ms);
    if (V.use_queue)
        vlog("mp4: %d slots; on screen %d, passed over %d, starved %d, longest between pictures %u ms; slowest read %u ms, decode %u ms",
             V.q.nslots, s_ui_shown, V.q.skipped, s_ui_starved, s_ui_gap_max / 1000, read_max / 1000, decode_max / 1000);
    if (V.use_queue)
        vlog("mp4: screen loop avg %u ms, longest %u ms; drawing avg %u ms, longest %u ms; colour convert avg %u ms, longest %u ms",
             s_tick_n ? s_tick_sum / s_tick_n / 1000 : 0, s_tick_max / 1000,
             s_tick_n ? s_ge_sum / s_tick_n / 1000 : 0, s_ge_max / 1000,
             csc_n ? csc_sum / csc_n / 1000 : 0, csc_max / 1000);
done:
    free(window);
    V.stop = 1; /* the audio thread too */
    if (V.athread >= 0) {
        SceUInt to = 1000000;
        sceKernelWaitThreadEnd(V.athread, &to);
        sceKernelDeleteThread(V.athread);
        V.athread = -1;
        sceIoClose(A.fd);
    }
    dec_close(&dec);
    free(sbuf);
    free(xbuf);
    sceIoClose(fd);
    mp4_close(&m);
    hold_off();
    return 0;
}

/* ------------------------------------------------- PMF video (psp5's path) */

#define PACKET 2048
#define RB_PACKETS 64

static SceUID s_pmf_fd = -1;
static u32 s_pmf_pos, s_pmf_end;

static SceInt32 ring_cb(ScePVoid data, SceInt32 packets, ScePVoid param)
{
    u32 left = s_pmf_end > s_pmf_pos ? (s_pmf_end - s_pmf_pos) / PACKET : 0;
    if ((u32)packets > left) packets = left;
    if (packets <= 0) return 0;
    int got;
    if (s_pmf_mem) {
        got = packets * PACKET;
        memcpy(data, s_pmf_mem + s_pmf_pos, got);
    } else {
        sceIoLseek32(s_pmf_fd, s_pmf_pos, PSP_SEEK_SET);
        got = sceIoRead(s_pmf_fd, data, packets * PACKET);
    }
    if (got <= 0) return 0;
    packets = got / PACKET;
    sceKernelDcacheWritebackRange(data, packets * PACKET); /* the ringbuffer is read past the cache */
    s_pmf_pos += packets * PACKET;
    return packets;
}

/* Plays the stream once; returns the pictures shown. */
static int pmf_session(const uint8_t *hdr, u32 file_size)
{
    int r = sceMpegInit();
    vlog("sceMpegInit = %08X", r);
    SceMpegRingbuffer rb;
    SceMpeg mpeg;
    int rb_size = sceMpegRingbufferQueryMemSize(RB_PACKETS), m_size = sceMpegQueryMemSize(0);
    void *rb_data = rb_size > 0 ? memalign(64, rb_size) : NULL, *m_data = m_size > 0 ? memalign(64, m_size) : NULL;
    int frames = 0;
    if (!rb_data || !m_data || sceMpegRingbufferConstruct(&rb, RB_PACKETS, rb_data, rb_size, ring_cb, NULL) < 0) {
        fail("ringbuffer failed", 0);
        goto out;
    }
    r = sceMpegCreate(&mpeg, m_data, m_size, &rb, 512, 0, 0);
    vlog("sceMpegCreate = %08X", r);
    if (r >= 0) {
        SceMpegAvcMode mode = {-1, SCE_MPEG_AVC_FORMAT_8888};
        sceMpegAvcDecodeMode(&mpeg, &mode);
        SceInt32 offset = 0, size = 0;
        int qo = sceMpegQueryStreamOffset(&mpeg, (ScePVoid)hdr, &offset), qs = sceMpegQueryStreamSize((ScePVoid)hdr, &size);
        vlog("pmf: stream offset %08X (%d), size %08X (%d)", qo, offset, qs, size);
        if (qo >= 0 && qs >= 0 && offset > 0 && (u32)offset < file_size) {
            s_pmf_pos = offset;
            s_pmf_end = (u32)(offset + size) <= file_size ? (u32)(offset + size) : file_size;
            SceMpegStream *vs = sceMpegRegistStream(&mpeg, 0, 0);
            void *es = vs ? sceMpegMallocAvcEsBuf(&mpeg) : NULL;
            SceMpegAu au;
            if (es && sceMpegInitAu(&mpeg, es, &au) >= 0) {
                V.state = ST_PLAYING;
                int fails = 0, first = 1;
                u32 last_pts = 0;
                clock_reset(0);
                int t_ms = 0;
                while (!V.stop) {
                    int avail = sceMpegRingbufferAvailableSize(&rb);
                    if (avail > 0) sceMpegRingbufferPut(&rb, avail, avail);
                    if (sceMpegGetAvcAu(&mpeg, vs, &au, NULL) < 0) {
                        if (s_pmf_pos >= s_pmf_end && ++fails > 2) break; /* the end */
                        sceKernelDelayThread(5000);
                        continue;
                    }
                    fails = 0;
                    u32 step = au.iPts - last_pts; /* 90 kHz; without a sane one, 29.97 fps */
                    if (!first) t_ms += step > 0 && step < 90 * 200 ? (int)(step / 90) : 33;
                    first = 0;
                    last_pts = au.iPts;
                    int next = free_slot();
                    if (next < 0) break;
                    void *dst = V.buf[next];
                    SceInt32 got = 0;
                    r = sceMpegAvcDecode(&mpeg, &au, 512, &dst, &got);
                    if (r >= 0 && got) {
                        if (V.preview != 2)
                            wait_until(t_ms);
                        V.pos_ms = t_ms;
                        V.published = next;
                        frames++;
                        if (V.preview == 2 && frames >= 12) { /* past a fade-in */
                            V.grabbed = 1;
                            break;
                        }
                    }
                }
                if (!V.stop) V.state = ST_ENDED;
            } else {
                fail("PMF stream setup failed", 0);
            }
            if (es) sceMpegFreeAvcEsBuf(&mpeg, es);
            if (vs) sceMpegUnRegistStream((SceMpeg)&mpeg, vs); /* a pointer, as psp5 found */
        } else {
            fail("no video stream in this PMF", qo);
        }
        sceMpegDelete(&mpeg);
    } else {
        fail("sceMpegCreate failed", r);
    }
    sceMpegRingbufferDestruct(&rb);
out:
    vlog("pmf: stopped after %d pictures", frames);
    sceMpegFinish();
    free(rb_data);
    free(m_data);
    return frames;
}

static int pmf_thread(SceSize args, void *argp)
{
    static uint8_t hdr[PACKET] __attribute__((aligned(64)));
    hold_on();
    u32 file_size = 0;
    int ok;
    if (s_pmf_mem) {
        ok = s_pmf_len >= PACKET && !memcmp(s_pmf_mem, "PSMF", 4);
        if (ok) memcpy(hdr, s_pmf_mem, PACKET);
        file_size = s_pmf_len;
    } else {
        s_pmf_fd = sceIoOpen(V.path, PSP_O_RDONLY, 0);
        ok = s_pmf_fd >= 0 && sceIoRead(s_pmf_fd, hdr, PACKET) == PACKET && !memcmp(hdr, "PSMF", 4);
        if (ok) file_size = sceIoLseek32(s_pmf_fd, 0, PSP_SEEK_END);
    }
    if (!ok) {
        fail("not a PMF video", 0);
        if (s_pmf_fd >= 0) sceIoClose(s_pmf_fd);
        s_pmf_fd = -1;
        hold_off();
        return 0;
    }
    int w = hdr[142] * 16, h = hdr[143] * 16;
    vlog("pmf: %dx%d, %u bytes", w, h, file_size);
    /* The XMB's mpeg_vsh, the same as for MP4: whichever sceMpeg is loaded first is
     * the one XLT's imports bind to, and only mpeg_vsh has sceMpegGetAvcNalAu.
     * Loading the plain MPEGBASE here (as before) broke every MP4 played after a
     * game icon. MPEGBASE stays as the fallback. */
    if (load_mp4_modules() != 0 && s_vsh_mod < 0) {
        int r = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
        vlog("sceUtilityLoadModule(AVCODEC) = %08X", r);
        r = sceUtilityLoadModule(PSP_MODULE_AV_MPEGBASE);
        vlog("sceUtilityLoadModule(MPEGBASE) = %08X", r);
    }
    if (w <= 0 || w > 512 || h <= 0 || h > 272 || alloc_frames(512, h, FRAMES_MIN) != 0) {
        fail("unsupported PMF size", 0);
    } else {
        V.fw = w;
        V.fh = h;
        /* a preview starts over at the end, like the XMB's icons */
        while (pmf_session(hdr, file_size) > 0 && V.preview == 1 && !V.stop)
            ;
    }
    if (s_pmf_fd >= 0) sceIoClose(s_pmf_fd);
    s_pmf_fd = -1;
    hold_off();
    return 0;
}

/* Every player thread ends through here, so the UI can tell when it's gone. */
static int mp4_entry(SceSize args, void *argp)
{
    mp4_thread(args, argp);
    V.thread_done = 1;
    return 0;
}

static int pmf_entry(SceSize args, void *argp)
{
    pmf_thread(args, argp);
    V.thread_done = 1;
    return 0;
}

/* -------------------------------------------------------------------- UI */

static void free_pmf_mem(void)
{
    free(s_pmf_mem);
    s_pmf_mem = NULL;
    s_pmf_len = 0;
}

static void stop_threads(void)
{
    V.stop = 1;
    V.paused = 0;
    if (V.vthread >= 0) {
        SceUInt to = 3000000;
        sceKernelWaitThreadEnd(V.vthread, &to);
        sceKernelDeleteThread(V.vthread);
        V.vthread = -1;
    }
    if (V.sthread >= 0) { /* it looks at V.stop between lines */
        SceUInt to = 2000000;
        sceKernelWaitThreadEnd(V.sthread, &to);
        sceKernelDeleteThread(V.sthread);
        V.sthread = -1;
    }
    free_frames();
    free_pmf_mem();
    V.preview = 0;
}

static volatile int s_slept, s_woke, s_slept_paused; /* set by the power callback */
static int s_switch_track = -1, s_switch_ms; /* △: reopen at s_switch_ms with this audio track */
static void job_save_cfg(void *arg) { (void)arg; globalcfg_save(); }

/* Up next: when a video ends, the next one in its folder (by name, the way the
 * Files tab sorts it) starts after a short countdown; X plays it now, O stops. */
#define UPNEXT_FRAMES (5 * 60)
static struct {
    int on, frames, last_sec;
    char path[200], title[128];
} U;

/* The video after cur in its folder: the smallest name above it. 0 if none. */
static int find_next(const char *cur, char *out, int n, char *title, int tn)
{
    const char *slash = strrchr(cur, '/');
    if (!slash)
        return 0;
    char dir[200], best[200] = "";
    snprintf(dir, sizeof(dir), "%.*s", (int)(slash - cur), cur);
    const char *me = slash + 1;
    SceUID d = sceIoDopen(dir);
    if (d < 0)
        return 0;
    SceIoDirent de;
    memset(&de, 0, sizeof(de));
    while (sceIoDread(d, &de) > 0) {
        const char *name = de.d_name;
        char p[460]; /* dir (< 200) + name (< 256) */
        if (!FIO_S_ISDIR(de.d_stat.st_mode) && strlen(dir) + 1 + strlen(name) < sizeof(U.path)) {
            snprintf(p, sizeof(p), "%s/%s", dir, name);
            if (!hidden_get(p)) /* removed from the library: skipped */
                vnext_consider(me, name, best, sizeof(best));
        }
        memset(&de, 0, sizeof(de));
    }
    sceIoDclose(d);
    if (!best[0])
        return 0;
    if (snprintf(out, n, "%s/%s", dir, best) >= n)
        return 0;
    const char *dot = strrchr(best, '.');
    snprintf(title, tn, "%.*s", dot ? (int)(dot - best) : (int)strlen(best), best);
    return 1;
}
static int s_wake_frames, s_pause_after;

void video_open(const Entry *e)
{
    if (sysctl_usb_active()) {
        ui_toast("Turn USB off to play videos");
        return;
    }
    s_pause_after = 0;
    U.on = 0;
    int switching = s_switch_track >= 0 && !strcmp(e->path, V.path);
    int keep_sub = switching ? subs_id_now() : s_keep_sub;
    s_keep_sub = -2;
    stop_threads();
    music_shutdown(); /* the audio channel and the ME are needed here */
    snprintf(V.path, sizeof(V.path), "%s", e->path);
    snprintf(V.title, sizeof(V.title), "%s", e->title);
    const char *dot = strrchr(e->path, '.');
    V.pmf = dot && !strcasecmp(dot, ".pmf");
    V.state = ST_LOADING;
    V.stop = V.paused = 0;
    V.err[0] = 0;
    V.why[0] = 0;
    V.pos_ms = V.dur_ms = 0;
    V.seek_ms = 0;
    V.seek_from_ms = -1;
    V.seek_gen++;
    V.audio_running = 0;
    V.overlay = OVERLAY_FRAMES;
    V.resumed_ms = V.pmf ? 0 : vp_get(&s_vp, e->path, NULL); /* a PMF can't seek */
    if (V.resumed_ms) {
        V.seek_ms = V.resumed_ms - 3000 > 0 ? V.resumed_ms - 3000 : 0; /* a moment before, to pick up the thread */
        V.overlay = OVERLAY_FRAMES * 2;
    }
    V.saved_ms = V.resumed_ms;
    V.audio_track = -1;
    V.naudio = 0;
    if (switching) { /* another audio track: on from where it was, no "carrying on" offer */
        V.audio_track = s_switch_track;
        V.resumed_ms = 0;
        V.seek_ms = s_switch_ms;
        V.overlay = OVERLAY_FRAMES;
    }
    s_switch_track = -1;
    V.open = 1;
    spring_to(&V.t, FX(1));
    SceIoStat st; /* one log per XLT session; started afresh if it grows past 64 KB */
    if (sceIoGetstat(s_log_path, &st) >= 0 && st.st_size > 64 * 1024)
        sceIoRemove(s_log_path);
    vlog("--- open %s", e->path);
    load_subs();
    V.sub_want = keep_sub; /* the same video again: the same subtitles */
    V.sub_sel = -2;        /* until the MP4's tracks are known */
    if (keep_sub == -1 || V.pmf) /* off; a PMF has no others */
        V.sub_sel = V.sub_side && keep_sub != -1 ? 0 : -1;
    else if (V.sub_side && (keep_sub == -2 || keep_sub == SUB_SIDE)) /* the .srt wins */
        V.sub_sel = 0;
    V.thread_done = 0;
    /* MP4 decodes ahead, so it can run below the screen loop and never hold up a picture. */
    int prio = V.pmf ? 0x18 : sceKernelGetThreadCurrentPriority() + 2;
    vlog("video thread priority %02X", prio);
    V.vthread = sceKernelCreateThread("xl_video", V.pmf ? pmf_entry : mp4_entry, prio, 0x10000,
                                      PSP_THREAD_ATTR_USER, NULL);
    if (V.vthread < 0 || sceKernelStartThread(V.vthread, 0, NULL) < 0)
        fail("couldn't start the player", V.vthread);
}

void video_shutdown(void)
{
    remember_place(1); /* written now: a game or the PC is about to have the stick */
    U.on = 0;
    stop_threads();
    subs_free();
    V.open = 0;
    spring_snap(&V.t, 0);
}

int video_is_open(void) { return V.open || V.t.x > 0; }
int video_covers(void) { return V.open && V.t.x >= FX(1); }

static void seek(int delta_ms)
{
    if (V.pmf || V.state != ST_PLAYING)
        return;
    int target = V.pos_ms + delta_ms;
    if (target < 0) target = 0;
    if (V.dur_ms && target > V.dur_ms - 1000) target = V.dur_ms - 1000;
    V.seek_from_ms = delta_ms > 0 ? V.pos_ms : -1;
    V.seek_ms = target;
    V.pos_ms = target; /* so presses in a row add up */
    V.seek_gen++;
}

int video_input(unsigned int p)
{
    if (!V.open)
        return 0;
    if (U.on) { /* the Up next screen: X plays it now, O goes back to the list */
        if (p & (PSP_CTRL_CROSS | PSP_CTRL_START)) {
            U.frames = 0;
        } else if (p & PSP_CTRL_CIRCLE) {
            U.on = 0;
            V.open = 0;
            spring_to(&V.t, 0);
        }
        return 1;
    }
    V.overlay = OVERLAY_FRAMES;
    if (p & PSP_CTRL_CIRCLE) {
        remember_place(0);
        stop_threads();
        subs_free();
        V.open = 0;
        spring_to(&V.t, 0);
    } else if (p & PSP_CTRL_TRIANGLE && V.resumed_ms) { /* from the start after all */
        V.resumed_ms = 0;
        V.seek_ms = 0;
        V.seek_gen++;
    } else if (p & PSP_CTRL_TRIANGLE && V.naudio > 1 && V.state == ST_PLAYING) { /* the next audio track */
        int k = (V.audio_sel + 1) % V.naudio;
        char msg[80];
        snprintf(msg, sizeof(msg), "Audio: %s (%d/%d)", V.audio_label[k], k + 1, V.naudio);
        static Entry e;
        snprintf(e.path, sizeof(e.path), "%s", V.path);
        snprintf(e.title, sizeof(e.title), "%s", V.title);
        s_switch_track = k;
        s_switch_ms = V.pos_ms;
        remember_place(0);
        video_open(&e);
        ui_toast(msg);
        if (V.audio_lang[k][0] && strcmp(g_cfg.audio_lang, V.audio_lang[k])) { /* the next videos start in it too */
            snprintf(g_cfg.audio_lang, sizeof(g_cfg.audio_lang), "%s", V.audio_lang[k]);
            bg_post(job_save_cfg, NULL, 0);
        }
    } else if (p & PSP_CTRL_SQUARE && V.nsub) { /* off, then each source in turn */
        int n = V.nsub, k = V.sub_sel < 0 ? 0 : V.sub_sel + 1;
        if (k >= n) k = -1;
        V.sub_sel = k;
        char msg[80];
        if (k < 0) snprintf(msg, sizeof(msg), "Subtitles off");
        else if (n == 1) snprintf(msg, sizeof(msg), "Subtitles on");
        else snprintf(msg, sizeof(msg), "Subtitles: %s (%d/%d)", V.sub_label[k], k + 1, n);
        ui_toast(msg);
        /* the next videos start the same way (a .srt file has no language to keep) */
        const char *want = k < 0 ? (n > V.sub_side ? "off" : NULL) : k == 0 && V.sub_side ? NULL : V.sub_lang[k];
        if (want && strcmp(g_cfg.sub_lang, want)) {
            snprintf(g_cfg.sub_lang, sizeof(g_cfg.sub_lang), "%s", want);
            bg_post(job_save_cfg, NULL, 0);
        }
    } else if (p & (PSP_CTRL_CROSS | PSP_CTRL_START)) {
        if (V.state == ST_PLAYING) V.paused = !V.paused;
    } else if (p & PSP_CTRL_LEFT) seek(-10000);
    else if (p & PSP_CTRL_RIGHT) seek(10000);
    else if (p & PSP_CTRL_LTRIGGER) seek(-60000);
    else if (p & PSP_CTRL_RTRIGGER) seek(60000);
    return 1;
}

/* Power callback: the PSP is going to sleep. Files, the decoder and the audio
 * don't survive it, so the player stops cleanly and reopens on waking. */
void video_on_suspend(void)
{
    if (!V.open || V.preview || V.vthread < 0 || V.state != ST_PLAYING)
        return;
    s_slept_paused = V.paused;
    s_slept = 1;
    V.stop = 1;
    int i = 0;
    for (; i < 150 && !V.thread_done; i++)
        sceKernelDelayThread(10000);
    vlog_always("sleep: player stopped after %d ms at %d ms%s", i * 10, V.pos_ms, V.thread_done ? "" : " (still running)");
}

void video_on_resume(void)
{
    if (s_slept) {
        s_slept = 0;
        s_wake_frames = 0;
        s_woke = 1;
    }
}

/* Woken from sleep: the same video again, from the same place. */
static void reopen_after_sleep(void)
{
    static Entry e;
    int paused = s_slept_paused;
    s_woke = 0;
    s_keep_sub = subs_id_now();
    vlog_always("woke: reopening at %d ms%s", V.pos_ms, paused ? ", paused" : "");
    if (!V.pmf) { /* a PMF can't seek: it starts again */
        vp_set(&s_vp, V.path, V.pos_ms, V.dur_ms);
        vp_save(0);
    }
    snprintf(e.path, sizeof(e.path), "%s", V.path);
    snprintf(e.title, sizeof(e.title), "%s", V.title);
    video_open(&e);
    s_pause_after = paused;
}

int video_step(void)
{
    int old = V.t.x;
    spring_step(&V.t);
    int dirty = V.t.x != old;
    if (!V.open) {
        s_woke = 0;
        return dirty;
    }
    if (U.on) {
        if (U.frames <= 0) {
            Entry e;
            memset(&e, 0, sizeof(e));
            snprintf(e.path, sizeof(e.path), "%s", U.path);
            snprintf(e.title, sizeof(e.title), "%s", U.title);
            e.kind = KIND_VIDEO;
            vlog("up next: %s", U.path);
            video_open(&e);
            return 1;
        }
        U.frames--;
        int sec = (U.frames + 59) / 60;
        if (sec != U.last_sec) {
            U.last_sec = sec;
            dirty = 1;
        }
        return dirty;
    }
    if (s_woke) { /* the Memory Stick takes a moment to come back */
        SceIoStat st;
        if (++s_wake_frames >= 30 && (sceIoGetstat(V.path, &st) >= 0 || s_wake_frames >= 600)) {
            reopen_after_sleep();
            return 1;
        }
        return dirty;
    }
    if (s_pause_after && V.state == ST_PLAYING && V.shown >= 0) { /* it was paused when the PSP went to sleep */
        s_pause_after = 0;
        V.paused = 1;
        dirty = 1;
    }
    if (V.use_queue) /* the GPU is idle now */
        dirty |= queue_take();
    else if (V.published >= 0 && V.shown != V.published) { /* a new frame: take it (the GPU is idle now) */
        V.shown = V.published;
        dirty = 1;
    }
    if (V.overlay > 0 && --V.overlay == 0) {
        dirty = 1;
        V.resumed_ms = 0; /* the "from the start" offer goes with the overlay */
    }
    if (V.state != V.last_state || V.paused != V.last_paused) {
        V.last_state = V.state;
        V.last_paused = V.paused;
        dirty = 1;
    }
    if (V.overlay && V.pos_ms / 1000 != V.last_sec) {
        V.last_sec = V.pos_ms / 1000;
        dirty = 1;
    }
    if (V.state == ST_PLAYING && !V.paused && !V.pmf && (V.pos_ms - V.saved_ms > 30000 || V.saved_ms - V.pos_ms > 30000))
        remember_place(0); /* every 30 s, in case the battery runs out */
    if (V.state == ST_PLAYING && !V.paused && !V.preview) { /* no auto sleep or backlight off during a film; paused, they run as usual */
        static int ticks;
        if (++ticks >= 60) { /* about once a second is plenty: the firmware counts in minutes */
            ticks = 0;
            scePowerTick(PSP_POWER_TICK_ALL);
        }
    }
    if (subs_now()) { /* a new subtitle line shows or goes */
        static const SrtCue *shown;
        const SrtCue *c = srt_at(subs_now(), V.pos_ms);
        if (c != shown) { shown = c; dirty = 1; }
    }
    if (V.state == ST_ENDED) { /* back to the list when it's over */
        vp_forget(&s_vp, V.path); /* watched: nothing to carry on from */
        vp_save(0);
        stop_threads();
        subs_free();
        if (g_cfg.autoplay && !sysctl_usb_active() &&
            find_next(V.path, U.path, sizeof(U.path), U.title, sizeof(U.title))) {
            U.on = 1; /* the screen stays; the countdown runs in video_step */
            U.frames = UPNEXT_FRAMES;
            U.last_sec = -1;
            V.state = ST_IDLE;
        } else {
            V.open = 0;
            spring_to(&V.t, 0);
        }
        dirty = 1;
    }
    return dirty;
}

static void time_str(char *b, int n, int ms)
{
    int s = ms / 1000;
    if (s >= 3600) snprintf(b, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(b, n, "%d:%02d", s / 60, s % 60);
}

/* Text centred on the screen in lines of at most w, broken at spaces; returns the y under it. */
static int wrap_center(int font, int y, int w, const char *s, u32 c, int max_lines)
{
    char line[160];
    for (int l = 0; *s && l < max_lines; l++) {
        int n = 0, brk = 0;
        while (s[n] && n < (int)sizeof(line) - 1) {
            memcpy(line, s, n + 1);
            line[n + 1] = 0;
            if (gfx_text_width(font, line) > w) break;
            if (s[n] == ' ') brk = n;
            n++;
        }
        if (s[n] && brk > 0) n = brk; /* break at the last space that fits */
        if (n == 0) n = 1;
        snprintf(line, sizeof(line), "%.*s", n, s);
        gfx_text_center(font, SCREEN_W / 2, y, line, c);
        y += gfx_line_h(font);
        s += n;
        while (*s == ' ') s++;
    }
    return y;
}

/* Subtitle lines, white with a dark edge, centred, the last line's bottom at y.
 * Lines too wide are broken at spaces. */
static void draw_subtitle(const char *text, int bottom)
{
    char lines[8][160];
    int n = 0;
    const int w = SCREEN_W - 40, font = FONT_BODY, lh = gfx_line_h(FONT_BODY);
    for (const char *s = text; *s && n < 8;) {
        const char *nl = strchr(s, '\n');
        int len = nl ? (int)(nl - s) : (int)strlen(s);
        while (len > 0 && n < 8) { /* this source line, broken to fit */
            int take = len, brk = 0;
            char tmp[160];
            for (int k = 1; k <= len && k < (int)sizeof(tmp); k++) {
                memcpy(tmp, s, k);
                tmp[k] = 0;
                if (gfx_text_width(font, tmp) > w) { take = brk > 0 ? brk : k - 1; break; }
                if (s[k] == ' ') brk = k;
                take = k;
            }
            if (take <= 0) take = 1;
            snprintf(lines[n++], sizeof(lines[0]), "%.*s", take, s);
            s += take;
            len -= take;
            while (len > 0 && *s == ' ') { s++; len--; }
        }
        s = nl ? nl + 1 : s + len;
    }
    int y = bottom - n * lh;
    for (int i = 0; i < n; i++, y += lh) {
        static const int off[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, 1}, {-1, 1}, {1, -1}};
        for (int k = 0; k < 8; k++)
            gfx_text_center(font, SCREEN_W / 2 + off[k][0], y + off[k][1], lines[i], 0xE0000000u);
        gfx_text_center(font, SCREEN_W / 2, y, lines[i], C_WHITE);
    }
}

void video_draw(void)
{
    int t = V.t.x;
    if (t <= 0)
        return;
    if (t > 256) t = 256;
    gfx_set_opacity(t);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGB(0x000000));
    if (U.on) {
        int y = SCREEN_H / 2 - 46;
        gfx_text_center(FONT_SMALL, SCREEN_W / 2, y, "Up next", C_MUTED);
        y += gfx_line_h(FONT_SMALL) + 6;
        y = wrap_center(FONT_BOLD, y, SCREEN_W - 80, U.title, C_WHITE, 2) + 10;
        char c[32];
        int sec = (U.frames + 59) / 60;
        snprintf(c, sizeof(c), "Playing in %d", sec > 0 ? sec : 1);
        gfx_text_center(FONT_SMALL, SCREEN_W / 2, y, c, C_TEXT);
        int bw = 120, bx = (SCREEN_W - bw) / 2, by = y + gfx_line_h(FONT_SMALL) + 8;
        gfx_rrect(bx, by, bw, 4, 2, C_PILL);
        gfx_rrect(bx, by, bw * (UPNEXT_FRAMES - U.frames) / UPNEXT_FRAMES, 4, 2, C_FOCUS);
        Hint hints[2] = {
            {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Play now"},
            {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
        };
        ui_draw_hints(hints, 2, SCREEN_W / 2, 251);
        gfx_set_opacity(256);
        return;
    }
    if (V.shown >= 0 && V.buf[V.shown] && V.fw > 0 && V.fh > 0) {
        /* fit to 480x272, aspect kept */
        int dw = SCREEN_W, dh = SCREEN_W * V.fh / V.fw;
        if (dh > SCREEN_H) { dh = SCREEN_H; dw = SCREEN_H * V.fw / V.fh; }
        if (V.fw == 720 && V.fh == 480) { /* the PSP's wide format: 16:9, not square pixels */
            dw = SCREEN_W;
            dh = SCREEN_H;
        }
        int dx = (SCREEN_W - dw) / 2, dy = (SCREEN_H - dh) / 2;
        gfx_video_frame(V.buf[V.shown], V.tbw, V.fw, V.fh, dx, dy, dw, dh);
    }
    const char *msg = V.state == ST_LOADING && V.shown < 0 ? "Loading..." : V.state == ST_FAILED ? V.err : NULL;
    if (msg && V.state == ST_FAILED && V.why[0]) { /* what the PSP has against it, and the way out */
        int y = SCREEN_H / 2 - 52;
        gfx_text_center(FONT_BODY, SCREEN_W / 2, y, msg, C_TEXT);
        y += gfx_line_h(FONT_BODY) + 4;
        y = wrap_center(FONT_SMALL, y, SCREEN_W - 80, V.why, C_TEXT, 2) + 6;
        wrap_center(FONT_SMALL, y, SCREEN_W - 80, H264_PSP_FIX, C_MUTED, 4);
    } else if (msg) {
        gfx_text_center(FONT_BODY, SCREEN_W / 2, SCREEN_H / 2 - 16, msg, C_TEXT);
        if (V.state == ST_FAILED)
            gfx_text_center(FONT_SMALL, SCREEN_W / 2, SCREEN_H / 2 + 6, "Details are in cache/video.log", C_MUTED);
    }
    int bars = V.overlay > 0 || V.paused || V.state == ST_FAILED;
    const Srt *subs = subs_now();
    const SrtCue *cue = subs && V.state == ST_PLAYING ? srt_at(subs, V.pos_ms) : NULL;
    if (cue)
        draw_subtitle(cue->text, bars ? 222 : 264);
    if (bars) {
        gfx_rect(0, 0, SCREEN_W, 24, 0xA0000000u);
        if (V.naudio > 1) { /* which audio track, top right */
            const char *al = V.audio_label[V.audio_sel];
            int aw = gfx_text_width(FONT_SMALL, al);
            if (aw > 160) aw = 160;
            gfx_text_fit(FONT_SMALL, SCREEN_W - 12 - aw, 5, aw, al, C_MUTED);
            gfx_text_fit(FONT_SMALL, 12, 5, SCREEN_W - 36 - aw, V.title, C_TEXT);
        } else {
            gfx_text_fit(FONT_SMALL, 12, 5, SCREEN_W - 24, V.title, C_TEXT);
        }
        gfx_rect(0, 226, SCREEN_W, 46, 0xA0000000u);
        char a[16], b[16];
        time_str(a, sizeof(a), V.pos_ms);
        time_str(b, sizeof(b), V.dur_ms);
        gfx_text(FONT_SMALL, 12, 230, a, C_TEXT);
        gfx_text_right(FONT_SMALL, SCREEN_W - 12, 230, V.dur_ms ? b : "", C_MUTED);
        int bx = 60, bw = SCREEN_W - 120;
        gfx_rrect(bx, 236, bw, 4, 2, C_PILL);
        if (V.dur_ms > 0) {
            int f = (int)((long long)bw * (V.pos_ms < V.dur_ms ? V.pos_ms : V.dur_ms) / V.dur_ms);
            gfx_rrect(bx, 236, f, 4, 2, C_FOCUS);
        }
        Hint hints[6] = {
            {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Pause"},
            {-1, "L/R", 0, "1 min"},
            {-1, "<-/->", 0, "10 s"},
        };
        int hn = 3;
        if (V.resumed_ms) hints[hn++] = (Hint){ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "From start"};
        else if (V.naudio > 1) hints[hn++] = (Hint){ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Audio"};
        if (V.nsub > 1) hints[hn++] = (Hint){ICON_BTN_SQUARE, NULL, C_BTN_SQUARE, "Subtitles"};
        else if (V.nsub) hints[hn++] = (Hint){ICON_BTN_SQUARE, NULL, C_BTN_SQUARE, V.sub_sel >= 0 ? "Subs off" : "Subs on"};
        hints[hn++] = (Hint){ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Stop"};
        ui_draw_hints(hints, hn, SCREEN_W / 2, 251);
        if (V.resumed_ms) { /* where it carried on from */
            char r[48], at[16];
            time_str(at, sizeof(at), V.resumed_ms);
            snprintf(r, sizeof(r), "Carrying on from %s", at);
            int rw = gfx_text_width(FONT_SMALL, r) + 20;
            gfx_rrect((SCREEN_W - rw) / 2, 30, rw, gfx_line_h(FONT_SMALL) + 8, 8, 0xA0000000u);
            gfx_text_center(FONT_SMALL, SCREEN_W / 2, 34, r, C_WHITE);
        }
        if (V.paused)
            gfx_text_center(FONT_BOLD, SCREEN_W / 2, 100, "Paused", C_WHITE);
    }
    gfx_set_opacity(256);
}

/* ---------------------------------------------------------------- preview */

/* What to preview next, started once the old preview thread has gone. */
static struct {
    int pending, grab;
    char path[200];
    uint8_t *mem;
    u32 len;
} P;

/* The UI thread: frees a preview thread that has finished. */
static void reap(void)
{
    if (V.vthread < 0 || !V.thread_done)
        return;
    sceKernelWaitThreadEnd(V.vthread, NULL);
    sceKernelDeleteThread(V.vthread);
    V.vthread = -1;
    if (V.preview == 1)
        vlog_always("preview ended%s", s_pv_seen ? "" : " without a picture");
    free_frames();
    free_pmf_mem();
    V.preview = 0;
}

static void preview_start(void)
{
    P.pending = 0;
    V.preview = P.grab ? 2 : 1;
    V.grabbed = 0;
    s_pv_seen = 0;
    V.stop = V.paused = 0;
    V.err[0] = 0;
    V.why[0] = 0;
    V.state = ST_LOADING;
    V.pos_ms = V.dur_ms = V.seek_ms = 0;
    V.seek_gen++;
    V.audio_running = 0;
    V.thread_done = 0;
    snprintf(V.path, sizeof(V.path), "%s", P.path);
    s_pmf_mem = P.mem;
    s_pmf_len = P.len;
    P.mem = NULL;
    const char *dot = strrchr(V.path, '.');
    V.pmf = s_pmf_mem || (dot && !strcasecmp(dot, ".pmf"));
    vlog_always("--- %s %s", P.grab ? "thumbnail" : "preview", s_pmf_mem ? "(game icon)" : V.path);
    V.vthread = sceKernelCreateThread("xl_preview", V.pmf ? pmf_entry : mp4_entry, 0x30, 0x10000,
                                      PSP_THREAD_ATTR_USER, NULL);
    if (V.vthread < 0 || sceKernelStartThread(V.vthread, 0, NULL) < 0) {
        if (V.vthread >= 0) sceKernelDeleteThread(V.vthread);
        V.vthread = -1;
        free_pmf_mem();
        V.preview = 0;
    }
}

static void request(const char *path, uint8_t *mem, u32 len, int grab)
{
    if (V.open || sysctl_usb_active() || g_safe_start) {
        free(mem);
        return;
    }
    free(P.mem);
    snprintf(P.path, sizeof(P.path), "%s", path);
    P.mem = mem;
    P.len = len;
    P.grab = grab;
    P.pending = 1;
    if (V.vthread >= 0)
        V.stop = 1; /* the old one goes first; reap() frees it */
    reap();
    if (V.vthread < 0)
        preview_start();
}

void video_preview_file(const char *path) { request(path, NULL, 0, 0); }
void video_preview_pmf(const char *key, uint8_t *data, u32 len) { request(key, data, len, 0); }

int video_preview_idle(void) { return !V.open && V.vthread < 0 && !P.pending; }

int video_grab(const char *path)
{
    if (!video_preview_idle())
        return -1;
    request(path, NULL, 0, 1);
    return V.preview == 2 ? 0 : -1;
}

int video_grab_take(uint8_t *tex)
{
    if (V.preview != 2 || V.vthread < 0 || !V.thread_done)
        return 0;
    int ok = V.grabbed && V.published >= 0 && V.buf[V.published] &&
             thumb_from_frame((const uint8_t *)((u32)V.buf[V.published] | 0x40000000u), V.tbw, V.fw, V.fh, tex) == 0;
    reap();
    return ok ? 1 : -1;
}

void video_preview_stop(int wait)
{
    free(P.mem);
    P.mem = NULL;
    P.pending = 0;
    if (!V.preview || V.vthread < 0)
        return;
    if (V.preview == 2 && !wait)
        return; /* a thumbnail grab is short: let it finish */
    V.stop = 1;
    if (wait)
        stop_threads();
}

int video_preview_step(void)
{
    if (V.open)
        return 0;
    if (V.preview != 2)
        reap(); /* a grab's picture is taken (and freed) by video_grab_take */
    if (P.pending && V.vthread < 0)
        preview_start();
    if (V.preview == 1 && !V.stop && V.published >= 0 && V.shown != V.published) {
        V.shown = V.published;
        if (!s_pv_seen) {
            s_pv_seen = 1;
            vlog_always("preview: first picture %dx%d", V.fw, V.fh);
        }
        return 1;
    }
    return 0;
}

const char *video_preview_path(void) { return V.preview == 1 && !V.stop && V.shown >= 0 ? V.path : NULL; }

int video_preview_draw(int x, int y, int w, int h)
{
    if (V.preview != 1 || V.stop || V.shown < 0 || !V.buf[V.shown] || V.fw <= 0 || V.fh <= 0)
        return 0;
    int dw = w, dh = w * V.fh / V.fw; /* fitted, aspect kept, on black */
    if (dh > h) { dh = h; dw = h * V.fw / V.fh; }
    int dx = x + (w - dw) / 2, dy = y + (h - dh) / 2;
    if (dw < w || dh < h)
        gfx_rect(x, y, w, h, RGB(0x000000));
    gfx_video_frame(V.buf[V.shown], V.tbw, V.fw, V.fh, dx, dy, dw, dh);
    return 1;
}

void video_init(const char *app_dir)
{
    /* The decoder's 2 MB on a 4 MB boundary, kept for good: memalign needs about
     * 6 MB in one piece, which a long film's tables and big frames may not leave. */
    s_ddr = memalign(0x400000, 0x200000);
    snprintf(s_log_path, sizeof(s_log_path), "%.190scache/video.log", app_dir);
    sceIoRemove(s_log_path); /* a fresh log each time XLT starts */
    snprintf(s_vp_path, sizeof(s_vp_path), "%.190scache/videopos.txt", app_dir);
    SceUID fd = sceIoOpen(s_vp_path, PSP_O_RDONLY, 0);
    if (fd >= 0) {
        char *t = malloc(VP_MAX * 230);
        int n = t ? sceIoRead(fd, t, VP_MAX * 230) : -1;
        if (n > 0) vp_parse(&s_vp, t, n);
        free(t);
        sceIoClose(fd);
    }
}
