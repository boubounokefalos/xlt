/* XLT - system controls.
 *
 * Kernel functions are found with sctrlHENFindFunction and called through
 * kubridge's kuKernelCall, the way ARK's arkMenu (and psp5) do it, so the
 * launcher itself can stay a user-mode EBOOT.
 */
/* Parts of this file (brightness, kernel calls and USB mode) are adapted from psp5 by 08Weber,
 * https://github.com/08Weber/psp5, GPL-3.0. */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <psppower.h>
#include <psprtc.h>
#include <pspusb.h>
#include <pspusbstor.h>
#include <kubridge.h>
#include <systemctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sysctl.h"
#include "shotpng.h"
#include "ark.h"
#include "fatcount.h"

int scePowerRequestSuspend(void);
int scePowerRequestColdReset(int unk);
int scePowerRequestStandby(void);

#define NID_DISP_SET_LIGHT 0x9E3C6DC6 /* sceDisplaySetBrightness(level, 0) */
#define NID_DISP_GET_LIGHT 0x31C4BAA8 /* sceDisplayGetBrightness(&level, &unk) */
#define NID_POWER_CHARGE   0x90285886 /* scePowerBatteryDisableUsbCharging */
/* System volume lives in the kernel's sceImpose: ARK translates the old NIDs
 * on 6.60/6.61; the second NID of each pair is the 6.60 one, in case it doesn't
 * (psp5 reads the volume the same way, tested on hardware). */
#define NID_IMPOSE_GET     0x531C9778 /* sceImposeGetParam(param) */
#define NID_IMPOSE_GET_660 0xDC3BECFF
#define NID_IMPOSE_SET     0x810FB7FB /* sceImposeSetParam(param, value) */
#define NID_IMPOSE_SET_660 0x3C318569
#define USBSTOR_EF_DRIVERNAME "USBStorEFlash_Driver"
#define USB_PID 0x1c8

static void *s_light_set, *s_light_get, *s_no_charge;
static void *s_imp_get, *s_imp_set, *s_ctrl_peek;
#define NID_CTRL_PEEK 0x3A622550 /* sceCtrlPeekBufferPositive (psp5 reads HOME and NOTE with it) */
static int s_usb_on, s_usb_loaded, s_is_go;

static int kcall(void *f, u32 a1, u32 a2)
{
    KernelCallArg a;
    memset(&a, 0, sizeof(a));
    a.arg1 = a1;
    a.arg2 = a2;
    return kuKernelCall(f, &a) < 0 ? -1 : (int)a.ret1;
}

void sysctl_init(void)
{
    if (!ark_version())
        return;
    s_light_set = (void *)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay_driver", NID_DISP_SET_LIGHT);
    s_light_get = (void *)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay_driver", NID_DISP_GET_LIGHT);
    s_no_charge = (void *)sctrlHENFindFunction("scePower_Service", "scePower_driver", NID_POWER_CHARGE);
    s_is_go = kuKernelGetModel() == 4;
    s_imp_get = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", NID_IMPOSE_GET);
    if (!s_imp_get)
        s_imp_get = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", NID_IMPOSE_GET_660);
    s_ctrl_peek = (void *)sctrlHENFindFunction("sceController_Service", "sceCtrl_driver", NID_CTRL_PEEK);
    s_imp_set = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", NID_IMPOSE_SET);
    if (!s_imp_set)
        s_imp_set = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", NID_IMPOSE_SET_660);
}

/* ----------------------------------------------------------------- volume */

#define IMPOSE_MAIN_VOLUME 0x1 /* PSP_IMPOSE_MAIN_VOLUME */
#define IMPOSE_MUTE        0x8 /* PSP_IMPOSE_MUTE */

int sysctl_volume_get(int *muted)
{
    if (!s_imp_get)
        return -1;
    int v = kcall(s_imp_get, IMPOSE_MAIN_VOLUME, 0);
    if (v < 0 || v > VOLUME_MAX)
        return -1;
    if (muted) {
        int m = kcall(s_imp_get, IMPOSE_MUTE, 0);
        *muted = m > 0;
    }
    return v;
}

int sysctl_volume_settable(void) { return s_imp_get && s_imp_set; }

unsigned sysctl_kernel_buttons(void)
{
    if (!s_ctrl_peek)
        return 0;
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    return kcall(s_ctrl_peek, (u32)&pad, 1) < 0 ? 0 : pad.Buttons;
}

#define IMPOSE_AVLS 0x10 /* PSP_IMPOSE_AVLS */
int sysctl_avls(void)
{
    if (!s_imp_get)
        return -1;
    int v = kcall(s_imp_get, IMPOSE_AVLS, 0);
    return v < 0 ? -1 : v > 0;
}

int sysctl_impose_set(int param, int value)
{
    if (!s_imp_set)
        return -1;
    kcall(s_imp_set, (u32)param, (u32)value);
    return kcall(s_imp_get, (u32)param, 0) == value ? 0 : -1;
}

int sysctl_model(void) { return ark_version() ? kuKernelGetModel() : -1; }

const char *sysctl_model_name(void)
{
    int m = sysctl_model();
    return m == 0 ? "PSP-1000" : m == 1 ? "PSP-2000" : m == 4 ? "PSP Go" : m == 10 ? "PSP E1000"
         : (m == 2 || m == 3 || m == 5 || m == 6 || m == 7 || m == 8) ? "PSP-3000" : "PSP";
}

void sysctl_firmware(char *out, int n)
{
    unsigned v = (unsigned)sceKernelDevkitVersion();
    snprintf(out, n, "%u.%u%u", (v >> 24) & 0xF, (v >> 16) & 0xF, (v >> 8) & 0xF);
}

int sysctl_volume_set(int v, int muted)
{
    if (!sysctl_volume_settable())
        return -1;
    if (v < 0) v = 0;
    if (v > VOLUME_MAX) v = VOLUME_MAX;
    kcall(s_imp_set, IMPOSE_MAIN_VOLUME, (u32)v);
    kcall(s_imp_set, IMPOSE_MUTE, muted ? 1 : 0);
    int m = 0, now = sysctl_volume_get(&m);
    return now == v && m == (muted != 0) ? 0 : -1; /* AVLS may cap it: the caller re-reads */
}

/* ------------------------------------------------------------- brightness */

int sysctl_brightness_available(void) { return s_light_set && s_light_get; }

int brightness_to_hw(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return BRIGHTNESS_HW_MIN + (pct * (BRIGHTNESS_HW_MAX - BRIGHTNESS_HW_MIN) + 50) / 100;
}

int brightness_from_hw(int level)
{
    const int span = BRIGHTNESS_HW_MAX - BRIGHTNESS_HW_MIN;
    if (level <= BRIGHTNESS_HW_MIN) return 0;
    if (level >= BRIGHTNESS_HW_MAX) return 100;
    return ((level - BRIGHTNESS_HW_MIN) * 100 + span / 2) / span;
}

int sysctl_brightness_get(void)
{
    if (!s_light_get)
        return -1;
    int level = -1, unk = 0;
    KernelCallArg a;
    memset(&a, 0, sizeof(a));
    a.arg1 = (u32)&level;
    a.arg2 = (u32)&unk;
    if (kuKernelCall(s_light_get, &a) < 0 || level < 0 || level > 100)
        return -1;
    return brightness_from_hw(level);
}

int sysctl_brightness_hw(void)
{
    if (!s_light_get)
        return -1;
    int level = -1, unk = 0;
    KernelCallArg a;
    memset(&a, 0, sizeof(a));
    a.arg1 = (u32)&level;
    a.arg2 = (u32)&unk;
    return kuKernelCall(s_light_get, &a) < 0 ? -1 : level;
}

int sysctl_brightness_set(int pct)
{
    if (!s_light_set || pct < 0 || pct > 100)
        return -1;
    return kcall(s_light_set, (u32)brightness_to_hw(pct), 0) < 0 ? -1 : 0;
}

/* -------------------------------------------------------------------- usb */

int sysctl_usb_available(void) { return ark_version() != 0; }
int sysctl_usb_active(void) { return s_usb_on; }

int sysctl_usb_connected(void)
{
    return s_usb_on && (sceUsbGetState() & PSP_USB_CONNECTION_ESTABLISHED);
}

int sysctl_usb_start(void)
{
    static const char *mods[] = {
        "flash0:/kd/usb.prx",        "flash0:/kd/semawm.prx",    "flash0:/kd/usbstor.prx",
        "flash0:/kd/usbstormgr.prx", "flash0:/kd/usbstorms.prx", "flash0:/kd/usbstoreflash.prx",
        "flash0:/kd/usbstorboot.prx",
    };
    if (s_usb_on)
        return 0;
    if (!sysctl_usb_available())
        return -1;
    /* ARK's USB charging can block the connection on a PSP Go: turn it off first. */
    if (s_no_charge)
        kcall(s_no_charge, 0, 0);
    if (!s_usb_loaded) {
        for (unsigned i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
            if (!s_is_go && strstr(mods[i], "usbstoreflash"))
                continue;
            SceUID mid = kuKernelLoadModule(mods[i], 0, NULL);
            if (mid >= 0)
                sceKernelStartModule(mid, 0, NULL, NULL, NULL);
        }
        s_usb_loaded = 1;
    }
    if (sceUsbGetState() & PSP_USB_ACTIVATED) {
        sceUsbDeactivate(USB_PID);
        sceKernelDelayThread(10000);
    }
    sceUsbStart(PSP_USBBUS_DRIVERNAME, 0, 0);
    sceUsbStart(PSP_USBSTOR_DRIVERNAME, 0, 0);
    if (s_is_go)
        sceUsbStart(USBSTOR_EF_DRIVERNAME, 0, 0);
    sceUsbstorBootSetCapacity(0x800000);
    sceUsbActivate(USB_PID); /* as arkMenu: the result is ignored */
    s_usb_on = 1;
    return 0;
}

void sysctl_usb_stop(void)
{
    if (!s_usb_on)
        return;
    sceUsbDeactivate(USB_PID);
    if (s_is_go)
        sceUsbStop(USBSTOR_EF_DRIVERNAME, 0, 0);
    sceUsbStop(PSP_USBSTOR_DRIVERNAME, 0, 0);
    sceUsbStop(PSP_USBBUS_DRIVERNAME, 0, 0);
    s_usb_on = 0;
}

int sysctl_kcall_nid(const char *const *modules, const char *library, unsigned nid, unsigned arg,
                     int *result, const char **which)
{
    if (!ark_version())
        return -1;
    for (int i = 0; modules[i]; i++) {
        void *f = (void *)sctrlHENFindFunction((char *)modules[i], (char *)library, nid);
        if (!f)
            continue;
        int r = kcall(f, arg, 0);
        if (result) *result = r;
        if (which) *which = modules[i];
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ power */

void sysctl_sleep(void) { scePowerRequestSuspend(); }
void sysctl_shutdown(void) { scePowerRequestStandby(); }
void sysctl_restart(void) { scePowerRequestColdReset(0); }

/* ---------------------------------------------------------------- storage */

typedef struct {
    u32 max_clusters, free_clusters, max_sectors, sector_size, sectors_per_cluster;
} DevInfo;

/* Asking a big card for its free space walks its whole FAT: ~12 s on 128 GB,
 * during which the system is too busy even to draw. So it is measured only
 * when asked (Settings > About), saved with the date in cache/storage.txt,
 * and everything else reads that value. */
static struct { int ok; uint64_t free, total; } s_storage[2];
static int s_storage_date; /* yyyymmdd of the measurement, 0 = never */
static char s_storage_path[256];

static int storage_read(const char *dev, uint64_t *free_bytes, uint64_t *total_bytes);
static void storage_save(void);

void sysctl_storage_load(const char *app_dir)
{
    snprintf(s_storage_path, sizeof(s_storage_path), "%.200scache/storage.txt", app_dir);
    char text[160];
    SceUID fd = sceIoOpen(s_storage_path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return;
    int n = sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    text[n > 0 ? n : 0] = 0;
    unsigned long long f[2], t[2];
    int date = 0;
    if (sscanf(text, "%llu %llu %llu %llu %d", &f[0], &t[0], &f[1], &t[1], &date) >= 4) {
        s_storage_date = date;
        for (int k = 0; k < 2; k++) {
            s_storage[k].free = f[k];
            s_storage[k].total = t[k];
            s_storage[k].ok = t[k] > 0;
        }
    }
}

int sysctl_storage_date(void) { return s_storage_date; }

int sysctl_storage_measure(void)
{
    SceInt64 t0 = sceKernelGetSystemTimeWide();
    static const char *const dev[2] = {"ms0:", "ef0:"};
    for (int k = 0; k < 2; k++) {
        uint64_t f = 0, t = 0;
        s_storage[k].ok = storage_read(dev[k], &f, &t) == 0;
        s_storage[k].free = f;
        s_storage[k].total = t;
    }
    storage_save();
    return (int)((sceKernelGetSystemTimeWide() - t0) / 1000);
}

/* Today's date on the values, and both saved in cache/storage.txt. */
static void storage_save(void)
{
    ScePspDateTime now;
    memset(&now, 0, sizeof(now));
    sceRtcGetCurrentClockLocalTime(&now);
    s_storage_date = now.year * 10000 + now.month * 100 + now.day;
    char text[160];
    int n = snprintf(text, sizeof(text), "%llu %llu %llu %llu %d\n", (unsigned long long)s_storage[0].free,
                     (unsigned long long)(s_storage[0].ok ? s_storage[0].total : 0), (unsigned long long)s_storage[1].free,
                     (unsigned long long)(s_storage[1].ok ? s_storage[1].total : 0), s_storage_date);
    SceUID fd = s_storage_path[0] ? sceIoOpen(s_storage_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777) : -1;
    if (fd >= 0) {
        sceIoWrite(fd, text, n);
        sceIoClose(fd);
    }
}

/* Counting the Memory Stick's free clusters ourselves (fatcount.c): the FAT is
 * read from the raw partition in chunks by a thread, so the UI keeps running and
 * can show how far it got. The driver's own count (above) holds the whole PSP
 * instead. The block device is opened from user mode if the firmware lets us,
 * otherwise through the kernel's IoFileMgr (kubridge), as installers do. */
#define NID_IO_OPEN   0x109F50BC
#define NID_IO_READ   0x6A638D83
#define NID_IO_CLOSE  0x810C4BC3
#define NID_IO_LSEEK32 0x68963324
#define COUNT_CHUNK (64 * 1024)

static struct {
    volatile int state;     /* COUNT_* */
    volatile int permille;
    volatile int cancel;
    SceUID thread;
    int kernel;             /* the fd is a kernel one: every call goes through kubridge */
    SceUID fd;
    void *k_open, *k_read, *k_close, *k_seek;
    char how[48];           /* for the log: which device, which way */
    int ms;
} K;

static int kcall3(void *f, u32 a1, u32 a2, u32 a3)
{
    KernelCallArg a;
    memset(&a, 0, sizeof(a));
    a.arg1 = a1;
    a.arg2 = a2;
    a.arg3 = a3;
    return kuKernelCall(f, &a) < 0 ? -1 : (int)a.ret1;
}

static int raw_open(const char *dev)
{
    K.kernel = 0;
    K.fd = sceIoOpen(dev, PSP_O_RDONLY, 0);
    if (K.fd >= 0)
        return 0;
    if (!K.k_open) {
        K.k_open = (void *)sctrlHENFindFunction("sceIOFileManager", "IoFileMgrForKernel", NID_IO_OPEN);
        K.k_read = (void *)sctrlHENFindFunction("sceIOFileManager", "IoFileMgrForKernel", NID_IO_READ);
        K.k_close = (void *)sctrlHENFindFunction("sceIOFileManager", "IoFileMgrForKernel", NID_IO_CLOSE);
        K.k_seek = (void *)sctrlHENFindFunction("sceIOFileManager", "IoFileMgrForKernel", NID_IO_LSEEK32);
    }
    if (!K.k_open || !K.k_read || !K.k_close || !K.k_seek)
        return -1;
    K.fd = kcall3(K.k_open, (u32)dev, PSP_O_RDONLY, 0);
    K.kernel = 1;
    return K.fd >= 0 ? 0 : -1;
}

static int raw_read(void *buf, int n)
{
    return K.kernel ? kcall3(K.k_read, (u32)K.fd, (u32)buf, (u32)n) : sceIoRead(K.fd, buf, n);
}

static int raw_seek(u32 at)
{
    return K.kernel ? kcall3(K.k_seek, (u32)K.fd, at, PSP_SEEK_SET) : sceIoLseek32(K.fd, (int)at, PSP_SEEK_SET);
}

static void raw_close(void)
{
    if (K.fd < 0) return;
    if (K.kernel) kcall3(K.k_close, (u32)K.fd, 0, 0);
    else sceIoClose(K.fd);
    K.fd = -1;
}

static int count_thread(SceSize args, void *argp)
{
    SceInt64 t0 = sceKernelGetSystemTimeWide();
    static unsigned char buf[COUNT_CHUNK] __attribute__((aligned(64)));
    FatGeom g;
    u32 part = 0;
    int ok = -1;
    /* the partition itself; failing that the whole stick, through its MBR */
    if (raw_open("msstor0p1:") == 0 && raw_read(buf, 512) == 512 && fat_parse_boot(buf, &g) == 0) {
        snprintf(K.how, sizeof(K.how), "msstor0p1: (%s)", K.kernel ? "kernel" : "user");
        ok = 0;
    } else {
        raw_close();
        if (raw_open("msstor:") == 0 && raw_read(buf, 512) == 512) {
            int64_t lba = fat_mbr_first(buf);
            if (lba > 0 && lba < 0x400000 && raw_seek((u32)lba * 512) >= 0 && raw_read(buf, 512) == 512 &&
                fat_parse_boot(buf, &g) == 0) {
                part = (u32)lba * 512;
                snprintf(K.how, sizeof(K.how), "msstor: at sector %u (%s)", (unsigned)lba, K.kernel ? "kernel" : "user");
                ok = 0;
            }
        }
    }
    uint32_t free_n = 0, done = 0;
    if (ok == 0) {
        /* whole sectors: a block device reads nothing else */
        uint32_t start = g.fat_offset / g.sector * g.sector, skip = g.fat_offset - start;
        uint32_t want = skip + g.fat_bytes;
        if (raw_seek(part + start) < 0)
            ok = -1;
        while (ok == 0 && done < want) {
            if (K.cancel) { ok = -2; break; }
            uint32_t n = want - done < COUNT_CHUNK ? (want - done + g.sector - 1) / g.sector * g.sector : COUNT_CHUNK;
            int got = raw_read(buf, (int)n);
            if (got < (int)n) { ok = -1; break; }
            uint32_t lo = done < skip ? skip - done : 0; /* bytes of this chunk before the FAT */
            uint32_t at = done + lo - skip, len = n - lo;
            if (at + len > g.fat_bytes) len = g.fat_bytes - at;
            free_n += fat_count_free(&g, buf + lo, len, at);
            done += n;
            K.permille = (int)((uint64_t)done * 1000 / want);
        }
    }
    raw_close();
    K.ms = (int)((sceKernelGetSystemTimeWide() - t0) / 1000);
    if (ok == 0) {
        s_storage[0].free = (uint64_t)free_n * g.cluster;
        s_storage[0].total = (uint64_t)g.clusters * g.cluster;
        s_storage[0].ok = 1;
        storage_save();
        K.permille = 1000;
        K.state = COUNT_DONE;
    } else {
        K.state = ok == -2 ? COUNT_CANCELLED : COUNT_FAILED;
    }
    return 0;
}

int sysctl_storage_count_start(void)
{
    if (K.state == COUNT_RUNNING)
        return 0;
    if (K.thread > 0) {
        sceKernelWaitThreadEnd(K.thread, NULL);
        sceKernelDeleteThread(K.thread);
        K.thread = 0;
    }
    K.fd = -1;
    K.cancel = 0;
    K.permille = 0;
    K.how[0] = 0;
    K.state = COUNT_RUNNING;
    K.thread = sceKernelCreateThread("xl_space", count_thread, 0x30, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (K.thread < 0 || sceKernelStartThread(K.thread, 0, NULL) < 0) {
        K.thread = 0;
        K.state = COUNT_FAILED;
        return -1;
    }
    return 0;
}

int sysctl_storage_count_state(int *permille)
{
    if (permille) *permille = K.permille;
    return K.state;
}

void sysctl_storage_count_cancel(void) { K.cancel = 1; }

const char *sysctl_storage_count_how(int *ms)
{
    if (ms) *ms = K.ms;
    return K.how;
}

int sysctl_storage(const char *dev, uint64_t *free_bytes, uint64_t *total_bytes)
{
    int k = dev[0] == 'e'; /* ms0: / ef0: */
    *free_bytes = s_storage[k].free;
    *total_bytes = s_storage[k].total;
    return s_storage[k].ok ? 0 : -1;
}

static int storage_read(const char *dev, uint64_t *free_bytes, uint64_t *total_bytes)
{
    DevInfo info;
    DevInfo *p = &info;
    memset(&info, 0, sizeof(info));
    char d[8];
    snprintf(d, sizeof(d), "%.4s", dev); /* "ms0:" / "ef0:" */
    if (sceIoDevctl(d, 0x02425818, &p, sizeof(p), NULL, 0) < 0)
        return -1;
    uint64_t cluster = (uint64_t)info.sector_size * info.sectors_per_cluster;
    *free_bytes = cluster * info.free_clusters;
    *total_bytes = cluster * info.max_clusters;
    return 0;
}

/* ------------------------------------------------------------- screenshot */

typedef struct {
    SceUID fd;
} FileSink;

static int file_write(void *ctx, const void *data, int len)
{
    return sceIoWrite(((FileSink *)ctx)->fd, data, len) == len ? 0 : -1;
}

int sysctl_screenshot(const char *dev, char *name, int n)
{
    void *top;
    int bufw, fmt;
    if (sceDisplayGetFrameBuf(&top, &bufw, &fmt, PSP_DISPLAY_SETBUF_IMMEDIATE) < 0 || !top ||
        bufw < 480 || fmt < PSP_DISPLAY_PIXEL_FORMAT_565 || fmt > PSP_DISPLAY_PIXEL_FORMAT_8888)
        return -1;
    /* Copy the frame first (uncached view of VRAM): a video thread may flip
     * buffers while the PNG is being compressed. */
    const int W = 480, H = 272, bpp = fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 4 : 2;
    size_t bytes = (size_t)bufw * H * bpp;
    void *copy = malloc(bytes);
    if (!copy)
        return -1;
    memcpy(copy, (const void *)((u32)top | 0x40000000u), bytes);

    char dir[16], path[64];
    snprintf(dir, sizeof(dir), "%.4s/PICTURE", dev);
    sceIoMkdir(dir, 0777);
    ScePspDateTime t;
    sceRtcGetCurrentClockLocalTime(&t);
    snprintf(name, n, "xlt_%04d%02d%02d_%02d%02d%02d.png", t.year, t.month, t.day, t.hour, t.minute, t.second);
    snprintf(path, sizeof(path), "%s/%s", dir, name);

    FileSink sink;
    sink.fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    int ok = sink.fd >= 0;
    if (ok) {
        ok = shot_png(copy, fmt, bufw, W, H, file_write, &sink) == 0;
        sceIoClose(sink.fd);
        if (!ok)
            sceIoRemove(path); /* no half-written picture in Pictures */
    }
    free(copy);
    return ok ? 0 : -1;
}
