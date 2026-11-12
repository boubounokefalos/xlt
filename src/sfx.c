/* XLT - interface sounds. See sfx.h.
 *
 * The sounds are baked into build/sfx_data.h (44.1 kHz mono int16). One
 * hardware channel (not the SRC channel music and video use), and a thread
 * that sleeps until something is queued, then mixes up to VOICES sounds into
 * BUF-sample blocks (~12 ms). Every frame the UI may ask for several sounds
 * (a button and what it did); sfx_commit starts only the most important one,
 * so a press never makes a chord of clicks. */
#include <pspkernel.h>
#include <pspaudio.h>
#include <string.h>
#include "sfx.h"
#include "gamecfg.h"
#include "sfx_data.h"

#define BUF 512
#define VOICES 4

static const struct { const int16_t *pcm; int len; } s_snd[SFX_COUNT] = {SFX_TABLE};

/* Higher wins within a frame. */
static const unsigned char s_prio[SFX_COUNT] = {
    [SFX_NOTE0] = 8, [SFX_NOTE1] = 8, [SFX_NOTE2] = 8, [SFX_NOTE3] = 8,
    [SFX_MOVE] = 1, [SFX_CARD] = 2, [SFX_BACK] = 3, [SFX_OPEN] = 3, [SFX_CLOSE] = 3, [SFX_SELECT] = 4,
    [SFX_ON] = 5, [SFX_OFF] = 5, [SFX_ERROR] = 5, [SFX_KEY] = 5, [SFX_DEL] = 5, /* typing outranks the ✕ chime */ [SFX_SHUTTER] = 6, [SFX_LAUNCH] = 7,
};

static struct { int id, pos; } s_voice[VOICES]; /* id -1 = free */
static int s_ch = -1;
static SceUID s_thread = -1, s_lock = -1, s_work = -1;
static volatile int s_quit, s_active;
static int s_pending = -1;
static int16_t s_buf[BUF] __attribute__((aligned(64)));

static void lock(void) { sceKernelWaitSema(s_lock, 1, NULL); }
static void unlock(void) { sceKernelSignalSema(s_lock, 1); }

static int sfx_thread(SceSize args, void *argp)
{
    while (!s_quit) {
        if (!s_active) {
            sceKernelWaitSema(s_work, 1, NULL);
            continue;
        }
        int acc[BUF];
        memset(acc, 0, sizeof(acc));
        lock();
        int left = 0;
        for (int v = 0; v < VOICES; v++) {
            if (s_voice[v].id < 0)
                continue;
            const int16_t *p = s_snd[s_voice[v].id].pcm;
            int len = s_snd[s_voice[v].id].len, pos = s_voice[v].pos;
            int n = len - pos < BUF ? len - pos : BUF;
            for (int i = 0; i < n; i++)
                acc[i] += p[pos + i];
            s_voice[v].pos = pos + n;
            if (s_voice[v].pos >= len) s_voice[v].id = -1;
            else left++;
        }
        unlock();
        for (int i = 0; i < BUF; i++)
            s_buf[i] = (int16_t)(acc[i] > 32767 ? 32767 : acc[i] < -32767 ? -32767 : acc[i]);
        sceAudioOutputBlocking(s_ch, PSP_AUDIO_VOLUME_MAX, s_buf); /* returns as the block starts playing */
        s_active = left;
    }
    return 0;
}

static void start(int id)
{
    if (s_thread < 0 || id < 0 || id >= SFX_COUNT || !g_cfg.ui_sounds)
        return;
    lock();
    int slot = -1;
    for (int v = 0; v < VOICES && slot < 0; v++) /* a repeated tick restarts, rather than piling up */
        if (s_voice[v].id == id && (id == SFX_MOVE || id == SFX_CARD))
            slot = v;
    for (int v = 0; v < VOICES && slot < 0; v++)
        if (s_voice[v].id < 0)
            slot = v;
    if (slot < 0) { /* all busy: the one nearest its end makes room */
        slot = 0;
        for (int v = 1; v < VOICES; v++)
            if (s_snd[s_voice[v].id].len - s_voice[v].pos < s_snd[s_voice[slot].id].len - s_voice[slot].pos)
                slot = v;
    }
    s_voice[slot].id = id;
    s_voice[slot].pos = 0;
    int was = s_active;
    s_active = 1;
    unlock();
    if (!was)
        sceKernelSignalSema(s_work, 1);
}

void sfx_play(int id)
{
    if (id >= 0 && id < SFX_COUNT && (s_pending < 0 || s_prio[id] >= s_prio[s_pending]))
        s_pending = id;
}

void sfx_commit(void)
{
    if (s_pending >= 0)
        start(s_pending);
    s_pending = -1;
}

void sfx_now(int id) { start(id); }

void sfx_drain(int max_ms)
{
    for (int t = 0; t < max_ms && s_active; t += 10)
        sceKernelDelayThread(10000);
}

void sfx_init(void)
{
    for (int v = 0; v < VOICES; v++)
        s_voice[v].id = -1;
    s_ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, BUF, PSP_AUDIO_FORMAT_MONO);
    if (s_ch < 0)
        return;
    s_lock = sceKernelCreateSema("xl_sfxlock", 0, 1, 1, NULL);
    s_work = sceKernelCreateSema("xl_sfxwork", 0, 0, 1, NULL);
    s_quit = 0;
    s_thread = sceKernelCreateThread("xl_sfx", sfx_thread, 0x15, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (s_lock < 0 || s_work < 0 || s_thread < 0 || sceKernelStartThread(s_thread, 0, NULL) < 0) {
        if (s_thread >= 0) sceKernelDeleteThread(s_thread);
        s_thread = -1;
        sceAudioChRelease(s_ch);
        s_ch = -1;
    }
}

void sfx_shutdown(void)
{
    if (s_thread >= 0) {
        s_quit = 1;
        sceKernelSignalSema(s_work, 1);
        SceUInt timeout = 500000;
        sceKernelWaitThreadEnd(s_thread, &timeout);
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
    }
    if (s_ch >= 0) {
        for (int i = 0; i < 50 && sceAudioChRelease(s_ch) < 0; i++) /* busy while the last block plays */
            sceKernelDelayThread(10000);
        s_ch = -1;
    }
}
