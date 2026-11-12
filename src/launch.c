/* XLT - launch layer.
 *
 * Uses the same SystemControl calls and apitypes as ARK's own arkMenu
 * (Launcher-Menus/arkMenu/src/iso.cpp and eboot.cpp), also used by psp5:
 *   disc image  sctrlSESetUmdFile + BootConfFileIndex(driver), key "umdemu"
 *   homebrew    key "game"
 *   PSN         key "umdemu", driver MODE_UMD, PBOOT.PBP update if present
 *   PS1         key "pops"
 * Games on the PSP Go's internal storage (ef0:) need their own apitypes.
 * sctrlKernelLoadExecVSHWithApitype is exported to user mode by ARK, so a
 * normal (user-mode) EBOOT is enough.
 */
/* Parts of this file (the UMD boot paths and launch checks) are adapted from psp5 by 08Weber,
 * https://github.com/08Weber/psp5, GPL-3.0. */
#include <pspkernel.h>
#include <pspumd.h>
#include <psploadexec_kernel.h>
#include <systemctrl.h>
#include <systemctrl_se.h>
#include <stdio.h>
#include <string.h>
#include "launch.h"
#include "disc.h"

#define APITYPE_HOMEBREW      0x141
#define APITYPE_ISO           0x123
#define APITYPE_ISO_PBOOT     0x124
#define APITYPE_POPS          0x144
#define APITYPE_HOMEBREW_GO   0x152
#define APITYPE_ISO_GO        0x125
#define APITYPE_ISO_PBOOT_GO  0x126
#define APITYPE_POPS_GO       0x155

#define UMD_EBOOT_BIN "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN"
#define UMD_EBOOT_OLD "disc0:/PSP_GAME/SYSDIR/EBOOT.OLD"
#define UMD_BOOT_BIN  "disc0:/PSP_GAME/SYSDIR/BOOT.BIN"

_Static_assert(DRIVER_MARCH33 == (int)MODE_MARCH33 && DRIVER_NP9660 == (int)MODE_NP9660 &&
               DRIVER_INFERNO == (int)MODE_INFERNO, "driver ids must match ARK's modes");

/* LoadExec reads these after we call it: keep them static. */
static char s_path[256];
static char s_argp[256];
static struct SceKernelLoadExecVSHParam s_param;

int launch_supported(void)
{
    /* Without ARK these imports are unresolved and return 0 or an error. */
    return sctrlSEGetVersion() > 0 || sctrlHENGetVersion() > 0;
}

static int exists(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static int on_ef0(void) { return !strncmp(s_path, "ef0:", 4); }

static void set_param(const char *key, const char *argp)
{
    memset(&s_param, 0, sizeof(s_param));
    s_param.size = sizeof(s_param);
    s_param.key = (char *)key;
    s_param.argp = (void *)argp;
    s_param.args = strlen(argp) + 1;
}

/* An installed update: ms0:/PSP/GAME/<ID>/PBOOT.PBP (or ef0:). */
static int find_update(const char *id, char *out, int n)
{
    static const char *devs[] = {"ms0:", "ef0:"};
    if (!id[0])
        return 0;
    for (int i = 0; i < 2; i++) {
        snprintf(out, n, "%s/PSP/GAME/%s/PBOOT.PBP", devs[i], id);
        if (exists(out))
            return 1;
    }
    return 0;
}

static int launch_disc(const char *id, int driver)
{
    int apitype = on_ef0() ? APITYPE_ISO_GO : APITYPE_ISO;
    if (find_update(id, s_argp, sizeof(s_argp))) {
        apitype = on_ef0() ? APITYPE_ISO_PBOOT_GO : APITYPE_ISO_PBOOT;
    } else {
        /* Same boot file choice as arkMenu: EBOOT.OLD if patched, else a
         * non-empty BOOT.BIN, else EBOOT.BIN. */
        strcpy(s_argp, UMD_EBOOT_BIN);
        Disc d;
        uint32_t off, size;
        if (!disc_open(&d, s_path)) {
            if (!disc_find(&d, "PSP_GAME/SYSDIR/EBOOT.OLD", &off, &size)) {
                strcpy(s_argp, UMD_EBOOT_OLD);
            } else if (!disc_find(&d, "PSP_GAME/SYSDIR/BOOT.BIN", &off, &size) && size >= 4) {
                uint32_t magic = 0;
                if (!disc_read(&d, &magic, 4, off) && magic != 0)
                    strcpy(s_argp, UMD_BOOT_BIN);
            }
            disc_close(&d);
        }
    }
    set_param("umdemu", s_argp);
    sctrlSESetDiscType(PSP_UMD_TYPE_GAME);
    sctrlSESetBootConfFileIndex(driver);
    sctrlSESetUmdFile(s_path);
    return sctrlKernelLoadExecVSHWithApitype(apitype, s_path, &s_param);
}

static int launch_psn(void)
{
    int apitype = on_ef0() ? APITYPE_ISO_GO : APITYPE_ISO;
    strcpy(s_argp, UMD_EBOOT_BIN);
    char *slash = strrchr(s_path, '/');
    if (slash) {
        char pboot[256];
        snprintf(pboot, sizeof(pboot), "%.*s/PBOOT.PBP", (int)(slash - s_path), s_path);
        if (exists(pboot)) {
            strcpy(s_argp, pboot);
            apitype = on_ef0() ? APITYPE_ISO_PBOOT_GO : APITYPE_ISO_PBOOT;
        }
    }
    set_param("umdemu", s_argp);
    sctrlSESetBootConfFileIndex(MODE_UMD);
    sctrlSESetUmdFile("");
    return sctrlKernelLoadExecVSHWithApitype(apitype, s_path, &s_param);
}

/* The real disc: EBOOT.BIN with apitype 0x120, or an installed update
 * (PBOOT.PBP in PSP/GAME/<ID>) with 0x160, as arkMenu and psp5. */
#define APITYPE_UMD        0x120
#define APITYPE_UMD_UPDATE 0x160

static int launch_umd(const char *id)
{
    int apitype = APITYPE_UMD_UPDATE;
    if (!find_update(id, s_path, sizeof(s_path))) {
        apitype = APITYPE_UMD;
        strcpy(s_path, UMD_EBOOT_BIN);
    }
    set_param("game", s_path);
    sctrlSESetDiscType(PSP_UMD_TYPE_GAME);
    sctrlSESetBootConfFileIndex(MODE_UMD);
    sctrlSESetUmdFile("");
    return sctrlKernelLoadExecVSHWithApitype(apitype, s_path, &s_param);
}

static int launch_homebrew(void)
{
    set_param("game", s_path);
    return sctrlKernelLoadExecVSHWithApitype(on_ef0() ? APITYPE_HOMEBREW_GO : APITYPE_HOMEBREW, s_path, &s_param);
}

static int launch_pops(void)
{
    set_param("pops", s_path);
    return sctrlKernelLoadExecVSHWithApitype(on_ef0() ? APITYPE_POPS_GO : APITYPE_POPS, s_path, &s_param);
}

int launch_entry(const Entry *e, int driver)
{
    if (!launch_supported())
        return -1;
    char id[sizeof(e->id)];
    int kind = e->kind;
    snprintf(s_path, sizeof(s_path), "%s", e->path);
    memcpy(id, e->id, sizeof(id));
    if (driver != DRIVER_MARCH33 && driver != DRIVER_NP9660 && driver != DRIVER_INFERNO)
        driver = DRIVER_DEFAULT;

    switch (kind) {
    case KIND_ISO:
    case KIND_CSO:
    case KIND_ZSO: launch_disc(id, driver); break;
    case KIND_PSN: launch_psn(); break;
    case KIND_PS1: launch_pops(); break;
    case KIND_HOMEBREW: launch_homebrew(); break;
    case KIND_UMD: launch_umd(id); break;
    default: return -1;
    }
    return -2; /* LoadExec only returns if it failed */
}
