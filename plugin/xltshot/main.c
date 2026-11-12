/* xltshot - screenshots in games, for XLT.
 *
 * A kernel plugin ARK loads at the "game" runlevel (XLT installs it, see
 * src/shotplug.c). L+R+START saves the screen as a PNG in PICTURE/, like XLT's
 * own screenshots. While L and R are held, START is hidden from the game, so
 * the combination doesn't also open its pause menu. The game's threads are
 * paused while the picture is read and written, so it is one whole frame; a
 * short white edge says it was taken.
 *
 * Memory: the PNG encoder's working memory (~40 KB) is taken only for the shot,
 * from the kernel partition or, failing that, the user one. Everything is logged
 * to SEPLUGINS/xltshot.log on the same device. */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <psprtc.h>
#include <pspsysmem_kernel.h>
#include <psploadcore.h>
#include <stddef.h>
/* from the kernel's SysclibForKernel (libpspsysclib_kernel_660), which the SDK doesn't declare */
int snprintf(char *buf, size_t n, const char *fmt, ...);
#include <systemctrl.h>
#include <string.h>
#include "pngout.h"

PSP_MODULE_INFO("XLTShot", PSP_MODULE_KERNEL, 1, 0);

#define NID_CTRL_PEEK      0x3A622550 /* sceCtrlPeekBufferPositive */
#define NID_CTRL_INTERCEPT 0x7CA723DC /* sceCtrlSetButtonMasks / Intercept */
#define NID_DISP_GETBUF    0xEEDA2E54 /* sceDisplayGetFrameBuf */
#define NID_RTC_LOCALTIME  0xE7C27D1B /* sceRtcGetCurrentClockLocalTime */
#define COMBO (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)
#define POLL_US 20000

static int (*s_peek)(SceCtrlData *, int);
static u32 (*s_intercept)(u32, u32);
static int (*s_getbuf)(void **, int *, int *, int);
static int (*s_localtime)(ScePspDateTime *);
static char s_dev[8] = "ms0:";
static SceUID s_thread = -1;
static volatile int s_run = 1;

/* ------------------------------------------------------------------ log */

static void log_line(const char *line, int truncate)
{
    char p[64];
    snprintf(p, sizeof(p), "%sSEPLUGINS/xltshot.log", s_dev);
    SceUID fd = sceIoOpen(p, PSP_O_WRONLY | PSP_O_CREAT | (truncate ? PSP_O_TRUNC : PSP_O_APPEND), 0777);
    if (fd < 0) return;
    sceIoWrite(fd, line, strlen(line));
    sceIoWrite(fd, "\n", 1);
    sceIoClose(fd);
}
/* (the kernel's C library has snprintf but no vsnprintf) */
#define LOG(...) do { char l_[176]; snprintf(l_, sizeof(l_), __VA_ARGS__); log_line(l_, 0); } while (0)

/* zlib's own allocator is never used (pngout gives it the block), but it is linked */
void *malloc(size_t n) { (void)n; return NULL; }
void *calloc(size_t n, size_t m) { (void)n; (void)m; return NULL; }
void free(void *p) { (void)p; }

/* --------------------------------------------------------------- threads */

#define MAX_THREADS 96
static SceUID s_paused[MAX_THREADS];
static int s_npaused;

/* Pauses every user thread (the game's), so the frame stays as it is. */
static void pause_game(void)
{
    static SceUID ids[MAX_THREADS];
    int n = 0;
    s_npaused = 0;
    if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, MAX_THREADS, &n) < 0)
        return;
    SceUID me = sceKernelGetThreadId();
    for (int i = 0; i < n && i < MAX_THREADS; i++) {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (ids[i] == me || sceKernelReferThreadStatus(ids[i], &info) < 0)
            continue;
        if (!(info.attr & PSP_THREAD_ATTR_USER) || (info.status & PSP_THREAD_SUSPEND))
            continue;
        if (sceKernelSuspendThread(ids[i]) >= 0)
            s_paused[s_npaused++] = ids[i];
    }
}

static void resume_game(void)
{
    for (int i = 0; i < s_npaused; i++)
        sceKernelResumeThread(s_paused[i]);
    s_npaused = 0;
}

/* ------------------------------------------------------------ the picture */

typedef struct { u8 *vram; int stride, fmt; } Frame;

/* One row of the frame as RGB. */
static void row_rgb(const Frame *f, int y, u8 *out)
{
    if (f->fmt == PSP_DISPLAY_PIXEL_FORMAT_8888) {
        const u32 *p = (const u32 *)f->vram + y * f->stride;
        for (int x = 0; x < 480; x++, out += 3) {
            u32 c = p[x];
            out[0] = c & 0xFF;
            out[1] = c >> 8 & 0xFF;
            out[2] = c >> 16 & 0xFF;
        }
        return;
    }
    const u16 *p = (const u16 *)f->vram + y * f->stride;
    for (int x = 0; x < 480; x++, out += 3) {
        u32 c = p[x], r, g, b;
        if (f->fmt == PSP_DISPLAY_PIXEL_FORMAT_565) {
            r = c & 31; g = c >> 5 & 63; b = c >> 11 & 31;
            out[0] = r << 3 | r >> 2; out[1] = g << 2 | g >> 4; out[2] = b << 3 | b >> 2;
        } else if (f->fmt == PSP_DISPLAY_PIXEL_FORMAT_5551) {
            r = c & 31; g = c >> 5 & 31; b = c >> 10 & 31;
            out[0] = r << 3 | r >> 2; out[1] = g << 3 | g >> 2; out[2] = b << 3 | b >> 2;
        } else { /* 4444 */
            r = c & 15; g = c >> 4 & 15; b = c >> 8 & 15;
            out[0] = r * 17; out[1] = g * 17; out[2] = b * 17;
        }
    }
}

static int write_cb(void *ctx, const void *data, int len)
{
    return sceIoWrite(*(SceUID *)ctx, data, len) == len ? 0 : -1;
}

/* The game's ID from the disc (ISO or UMD), or "game". */
static void game_id(char *out, int n)
{
    snprintf(out, n, "game");
    SceUID fd = sceIoOpen("disc0:/UMD_DATA.BIN", PSP_O_RDONLY, 0);
    if (fd < 0)
        return;
    char b[16];
    int r = sceIoRead(fd, b, 10);
    sceIoClose(fd);
    if (r < 9)
        return;
    int k = 0;
    for (int i = 0; i < r && k < n - 1; i++)
        if ((b[i] >= 'A' && b[i] <= 'Z') || (b[i] >= '0' && b[i] <= '9'))
            out[k++] = b[i];
    out[k] = 0;
    if (!k) snprintf(out, n, "game");
}

/* A white edge round the screen for a moment, drawn into the frame on show. */
static void flash(const Frame *f)
{
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++) {
            if (y > 3 && y < 268 && x > 3 && x < 476) { x = 475; continue; }
            if (f->fmt == PSP_DISPLAY_PIXEL_FORMAT_8888) ((u32 *)f->vram)[y * f->stride + x] = 0xFFFFFFFF;
            else ((u16 *)f->vram)[y * f->stride + x] = 0xFFFF;
        }
}

static void take(void)
{
    SceInt64 t0 = sceKernelGetSystemTimeWide();
    void *top = NULL;
    int stride = 0, fmt = 0;
    if (!s_getbuf || s_getbuf(&top, &stride, &fmt, 0) < 0 || !top || stride < 480) {
        LOG("no frame buffer (%p, stride %d)", top, stride);
        return;
    }
    pause_game();
    Frame f = {(u8 *)((u32)top | 0x40000000), stride, fmt}; /* uncached: what is on the screen */

    char dir[32], path[96], id[16];
    snprintf(dir, sizeof(dir), "%sPICTURE", s_dev);
    sceIoMkdir(dir, 0777);
    game_id(id, sizeof(id));
    ScePspDateTime t;
    memset(&t, 0, sizeof(t));
    if (s_localtime && s_localtime(&t) >= 0 && t.year)
        snprintf(path, sizeof(path), "%s/%s_%04d%02d%02d_%02d%02d%02d.png", dir, id, t.year, t.month, t.day, t.hour,
                 t.minute, t.second);
    else /* no clock: a counter that doesn't repeat */
        snprintf(path, sizeof(path), "%s/%s_%u.png", dir, id, (unsigned)((u64)sceKernelGetSystemTimeWide() / 1000u));

    /* the encoder's memory, only for now */
    SceUID blk = sceKernelAllocPartitionMemory(1, "xltshot", PSP_SMEM_Low, PNGOUT_MEM, NULL);
    if (blk < 0)
        blk = sceKernelAllocPartitionMemory(2, "xltshot", PSP_SMEM_High, PNGOUT_MEM, NULL);
    int r = -1;
    if (blk >= 0) {
        SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) {
            static u8 row[480 * 3];
            PngOut png;
            r = pngout_begin(&png, sceKernelGetBlockHeadAddr(blk), 480, 272, write_cb, &fd);
            for (int y = 0; r == 0 && y < 272; y++) {
                row_rgb(&f, y, row);
                r = pngout_row(&png, row);
            }
            if (r == 0) r = pngout_end(&png);
            sceIoClose(fd);
            if (r != 0) sceIoRemove(path);
        }
        sceKernelFreePartitionMemory(blk);
    }
    int ms = (int)((u64)(sceKernelGetSystemTimeWide() - t0) / 1000u);
    if (r == 0) {
        flash(&f);
        sceKernelDelayThread(120000);
    }
    int n_paused = s_npaused;
    resume_game();
    LOG("%s %s: %d ms, format %d, stride %d, %d threads paused%s", r == 0 ? "saved" : "FAILED", path, ms, fmt, stride,
        n_paused, blk < 0 ? ", no memory" : "");
}

/* ----------------------------------------------------------------- thread */

/* XLT started from the XMB can count as homebrew and get this plugin too: XLT has
 * its own L+R+START, so then the plugin keeps out of the way (no masking, no shots). */
static int xlt_running(void) { return sceKernelFindModuleByName("XLT") != NULL; }

static int watch(SceSize args, void *argp)
{
    int held = 0, masked = 0, tick = 0, in_xlt = 0;
    sceKernelDelayThread(2000000); /* let the game start first */
    while (s_run) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        if (tick++ % 50 == 0) { /* about once a second */
            int x = xlt_running();
            if (x && !in_xlt) LOG("XLT is running: keeping out of its way");
            in_xlt = x;
            if (in_xlt && masked && s_intercept) { s_intercept(PSP_CTRL_START, 0); masked = 0; }
        }
        if (!in_xlt && s_peek && s_peek(&pad, 1) >= 0) {
            int lr = (pad.Buttons & COMBO) == COMBO;
            if (s_intercept && lr != masked) { /* START hidden from the game while L+R are held */
                s_intercept(PSP_CTRL_START, lr ? 1 : 0);
                masked = lr;
            }
            int combo = lr && (pad.Buttons & PSP_CTRL_START);
            if (combo && !held)
                take();
            held = combo;
        }
        sceKernelDelayThread(POLL_US);
    }
    if (masked && s_intercept)
        s_intercept(PSP_CTRL_START, 0);
    return 0;
}

int module_start(SceSize args, void *argp)
{
    SceUID d = sceIoDopen("ms0:/");
    if (d >= 0) sceIoDclose(d);
    else snprintf(s_dev, sizeof(s_dev), "ef0:"); /* a PSP Go without a card */
    s_peek = (void *)sctrlHENFindFunction("sceController_Service", "sceCtrl_driver", NID_CTRL_PEEK);
    s_intercept = (void *)sctrlHENFindFunction("sceController_Service", "sceCtrl_driver", NID_CTRL_INTERCEPT);
    s_getbuf = (void *)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay_driver", NID_DISP_GETBUF);
    s_localtime = (void *)sctrlHENFindFunction("sceRTC_Service", "sceRtc_driver", NID_RTC_LOCALTIME);
    if (!s_localtime)
        s_localtime = (void *)sctrlHENFindFunction("sceRTC_Service", "sceRtc", NID_RTC_LOCALTIME);
    char l[176];
    snprintf(l, sizeof(l), "xltshot 1.0: peek %s, start mask %s, frame buffer %s, clock %s", s_peek ? "ok" : "MISSING",
             s_intercept ? "ok" : "MISSING", s_getbuf ? "ok" : "MISSING", s_localtime ? "ok" : "MISSING");
    log_line(l, 1);
    s_thread = sceKernelCreateThread("xltshot", watch, 0x18, 0x2000, 0, NULL);
    if (s_thread >= 0)
        sceKernelStartThread(s_thread, 0, NULL);
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    s_run = 0;
    if (s_thread >= 0) {
        sceKernelWaitThreadEnd(s_thread, NULL);
        sceKernelDeleteThread(s_thread);
    }
    return 0;
}
