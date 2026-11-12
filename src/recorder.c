/* XLT - screen recorder. See recorder.h.
 *
 * Time is counted in vblanks (sceDisplayGetVcount), two to a frame. The UI thread
 * copies the displayed frame (16-bit, from VRAM) when a new one was drawn and the
 * encoder is free; the encoder thread (below the UI) writes "same as before" chunks
 * for the frames in between, then the JPEG. So the frame rate goes down by itself
 * when there is a lot to encode, and timing stays right. */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <psprtc.h>
#include <pspiofilemgr.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include "recorder.h"
#include "avi.h"
#include "recjpeg.h"
#include "sysctl.h"
#include "video.h"

#define FPS 30
#define QUALITY 70
#define MAX_SECONDS (60 * 60)
#define W 480
#define H 272
#define STRIDE 512

enum { MSG_NONE, MSG_SAVED, MSG_FULL, MSG_FAILED, MSG_LIMIT, MSG_SLEEP };

static struct {
    volatile int on, busy, quit, failed, full;
    SceUID thread, work, lock, fd;
    uint16_t *stage;
    Avi avi;
    uint32_t vstart, written, job_tick;
    int last_cap, pending, shown_sec, seconds;
    char name[48];
    u32 enc_sum, enc_n, enc_max, wr_max;
    volatile int msg;
} R = {.thread = -1, .work = -1, .lock = -1, .fd = -1};

static uint32_t now_tick(void) { return (uint32_t)(sceDisplayGetVcount() - R.vstart) / (60 / FPS); }

static int file_write(void *ctx, const void *p, uint32_t n)
{
    u32 t0 = sceKernelGetSystemTimeLow();
    int ok = sceIoWrite(R.fd, p, n) == (int)n;
    u32 t = sceKernelGetSystemTimeLow() - t0;
    if (t > R.wr_max) R.wr_max = t;
    return ok ? 0 : -1;
}

static int file_patch(void *ctx, uint32_t off, const void *p, uint32_t n)
{
    if (sceIoLseek32(R.fd, (int)off, PSP_SEEK_SET) < 0 || sceIoWrite(R.fd, p, n) != (int)n)
        return -1;
    sceIoLseek32(R.fd, 0, PSP_SEEK_END);
    return 0;
}

static void note(int r)
{
    if (r == AVI_ERR_FULL) R.full = 1;
    else if (r != AVI_OK) R.failed = 1;
}

static int encoder_thread(SceSize args, void *argp)
{
    for (;;) {
        sceKernelWaitSema(R.work, 1, NULL);
        if (R.quit)
            break;
        u32 t0 = sceKernelGetSystemTimeLow();
        uint8_t *jpeg;
        unsigned long len;
        if (rec_jpeg(R.stage, STRIDE, W, H, QUALITY, &jpeg, &len) == 0) {
            u32 t = sceKernelGetSystemTimeLow() - t0;
            R.enc_sum += t;
            R.enc_n++;
            if (t > R.enc_max) R.enc_max = t;
            int r = AVI_OK;
            while (r == AVI_OK && R.written < R.job_tick) { /* nothing new on screen in between */
                r = avi_frame(&R.avi, NULL, 0);
                if (r == AVI_OK) R.written++;
            }
            if (r == AVI_OK) r = avi_frame(&R.avi, jpeg, (uint32_t)len);
            if (r == AVI_OK) R.written++;
            note(r);
            free(jpeg);
        }
        R.busy = 0;
    }
    return 0;
}

/* Ends the recording (with R.lock held): the encoder finishes its frame, the rest is written. */
static void finish(int msg)
{
    if (!R.on)
        return;
    R.on = 0;
    for (int i = 0; i < 300 && R.busy; i++)
        sceKernelDelayThread(10000);
    R.quit = 1;
    sceKernelSignalSema(R.work, 1);
    SceUInt to = 2000000;
    sceKernelWaitThreadEnd(R.thread, &to);
    sceKernelDeleteThread(R.thread);
    R.thread = -1;
    uint32_t end = now_tick();
    int r = AVI_OK;
    while (!R.failed && !R.full && r == AVI_OK && R.written < end) { /* up to now, the last picture held */
        r = avi_frame(&R.avi, NULL, 0);
        if (r == AVI_OK) R.written++;
    }
    note(r);
    r = avi_end(&R.avi); /* even on a full stick: the index may still fit */
    sceIoClose(R.fd);
    R.fd = -1;
    free(R.stage);
    R.stage = NULL;
    R.seconds = (int)(R.written / FPS);
    video_note("recording %s: %d s, %u pictures in %u frames; JPEG avg %u ms, slowest %u ms; slowest write %u ms%s",
               R.name, R.seconds, R.avi.pictures, R.avi.frames, R.enc_n ? R.enc_sum / R.enc_n / 1000 : 0,
               R.enc_max / 1000, R.wr_max / 1000, r == AVI_OK ? "" : " (index not written)");
    R.msg = R.full ? MSG_FULL : R.failed ? MSG_FAILED : msg;
}

int rec_start(const char *dev)
{
    if (R.on)
        return 0;
    if (sysctl_usb_active())
        return -1;
    if (R.lock < 0) R.lock = sceKernelCreateSema("xl_reclock", 0, 1, 1, NULL);
    if (R.work < 0) R.work = sceKernelCreateSema("xl_recwork", 0, 0, 1, NULL);
    if (R.lock < 0 || R.work < 0)
        return -1;
    char dir[16], path[80];
    snprintf(dir, sizeof(dir), "%.4s/VIDEO", dev);
    sceIoMkdir(dir, 0777);
    ScePspDateTime t;
    sceRtcGetCurrentClockLocalTime(&t);
    snprintf(R.name, sizeof(R.name), "xlt_%04d%02d%02d_%02d%02d%02d.avi", t.year, t.month, t.day, t.hour, t.minute, t.second);
    snprintf(path, sizeof(path), "%s/%s", dir, R.name);
    R.stage = memalign(64, STRIDE * H * 2);
    R.fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    R.failed = R.full = R.quit = R.busy = 0;
    R.written = 0;
    R.enc_sum = R.enc_n = R.enc_max = R.wr_max = 0;
    if (!R.stage || R.fd < 0 || avi_begin(&R.avi, W, H, FPS, file_write, file_patch, NULL) != AVI_OK)
        goto fail;
    R.thread = sceKernelCreateThread("xl_rec", encoder_thread, 0x28, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (R.thread < 0 || sceKernelStartThread(R.thread, 0, NULL) < 0)
        goto fail;
    R.vstart = sceDisplayGetVcount();
    R.last_cap = -1;
    R.pending = 1; /* the screen as it is now */
    R.shown_sec = -1;
    R.msg = MSG_NONE;
    R.on = 1;
    return 0;
fail:
    if (R.thread >= 0) sceKernelDeleteThread(R.thread);
    R.thread = -1;
    avi_abandon(&R.avi);
    if (R.fd >= 0) {
        sceIoClose(R.fd);
        sceIoRemove(path);
    }
    R.fd = -1;
    free(R.stage);
    R.stage = NULL;
    return -1;
}

void rec_stop(void)
{
    if (R.lock < 0 || !R.on)
        return;
    sceKernelWaitSema(R.lock, 1, NULL);
    finish(MSG_SAVED);
    sceKernelSignalSema(R.lock, 1);
}

void rec_on_suspend(void)
{
    if (R.lock < 0 || !R.on)
        return;
    sceKernelWaitSema(R.lock, 1, NULL);
    finish(MSG_SLEEP);
    sceKernelSignalSema(R.lock, 1);
}

int rec_active(void) { return R.on; }
int rec_seconds(void) { return R.on ? (int)(now_tick() / FPS) : 0; }

int rec_frame(int drew)
{
    if (!R.on)
        return 0;
    sceKernelWaitSema(R.lock, 1, NULL);
    int dirty = 0;
    if (R.on) {
        uint32_t tick = now_tick();
        if (drew)
            R.pending = 1;
        if (R.failed || R.full) {
            finish(MSG_FAILED);
            dirty = 1;
        } else if (tick >= MAX_SECONDS * FPS) {
            finish(MSG_LIMIT);
            dirty = 1;
        } else {
            if (R.pending && !R.busy && (int)tick > R.last_cap) {
                void *top;
                int bufw, fmt;
                if (sceDisplayGetFrameBuf(&top, &bufw, &fmt, PSP_DISPLAY_SETBUF_NEXTFRAME) >= 0 && top &&
                    fmt == PSP_DISPLAY_PIXEL_FORMAT_565 && bufw == STRIDE) {
                    memcpy(R.stage, (const void *)((u32)top | 0x40000000u), STRIDE * H * 2); /* uncached: what the GE wrote */
                    R.job_tick = tick;
                    R.last_cap = (int)tick;
                    R.pending = 0;
                    R.busy = 1;
                    sceKernelSignalSema(R.work, 1);
                }
            }
            int sec = (int)(tick / FPS);
            if (sec != R.shown_sec) {
                R.shown_sec = sec;
                dirty = 1;
            }
        }
    }
    sceKernelSignalSema(R.lock, 1);
    return dirty;
}

int rec_take_message(char *out, int n)
{
    int m = R.msg;
    if (m == MSG_NONE)
        return 0;
    R.msg = MSG_NONE;
    int s = R.seconds;
    switch (m) {
    case MSG_FULL: snprintf(out, n, "Recording stopped: the Memory Stick is full"); break;
    case MSG_FAILED: snprintf(out, n, "Recording stopped: it couldn't be written"); break;
    case MSG_LIMIT: snprintf(out, n, "Recording stopped after an hour, saved to VIDEO"); break;
    case MSG_SLEEP: snprintf(out, n, "Recording saved to VIDEO before sleep (%d:%02d)", s / 60, s % 60); break;
    default: snprintf(out, n, "Recording saved to VIDEO (%d:%02d)", s / 60, s % 60); break;
    }
    return 1;
}
