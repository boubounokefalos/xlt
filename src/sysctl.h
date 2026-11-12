/* XLT - system controls for the control center (need ARK for kernel calls). */
#ifndef SYSCTL_H
#define SYSCTL_H

#include <stdint.h>

void sysctl_init(void);

/* Brightness as the user sees it: 0..100 %, mapped linearly onto the panel's
 * usable range 25..99 (0 % is still lit; 99 is the practical maximum on a
 * stock PSP-1000/2000 screen, see BetterBright). */
#define BRIGHTNESS_STEP 5   /* % per press in the control center */
#define BRIGHTNESS_HW_MIN 25
#define BRIGHTNESS_HW_MAX 99
int sysctl_brightness_available(void);
int sysctl_brightness_get(void);       /* 0..100, -1 if unknown */
int sysctl_brightness_set(int pct);    /* 0 on success */
int brightness_to_hw(int pct);
int brightness_from_hw(int level);
int sysctl_brightness_hw(void);        /* raw panel level, -1 if unknown */

/* System volume, the one the volume buttons change: 0..VOLUME_MAX. */
#define VOLUME_MAX 30
int sysctl_volume_get(int *muted);      /* -1 if it can't be read (no ARK) */
int sysctl_volume_settable(void);
int sysctl_volume_set(int v, int muted); /* 0 if the PSP took exactly that */
/* AVLS (the headphone limiter) on: 1, off: 0, -1 if it can't be read. It doesn't
 * lower the volume level: levels above AVLS_LEVEL just play no louder. */
int sysctl_avls(void);
#define AVLS_LEVEL 20
#define AVLS_MESSAGE "AVLS is on: the volume can't go past 20" /* with AVLS on, the level stops here */
/* The buttons only kernel mode sees (PSP_CTRL_VOLUP, VOLDOWN, NOTE, HOME...),
 * through sceCtrl_driver as psp5 does; 0 without ARK. */
unsigned sysctl_kernel_buttons(void);

/* sceImposeSetParam(param, value), read back. 0 if the PSP took it. */
int sysctl_impose_set(int param, int value);

int sysctl_model(void);              /* kuKernelGetModel(): 0 = PSP-1000, 4 = Go, -1 without ARK */
const char *sysctl_model_name(void); /* "PSP-3000", ... */
void sysctl_firmware(char *out, int n); /* "6.61" */

int sysctl_usb_available(void);
int sysctl_usb_start(void);            /* 0 on success */
void sysctl_usb_stop(void);
int sysctl_usb_active(void);
int sysctl_usb_connected(void);        /* cable in and the PC mounted it */

/* Calls a kernel function by NID with one argument. The function is looked up
 * with ARK's sctrlHENFindFunction in each of the NULL-terminated module names
 * until one has it; *which receives that name. -1 if none has it. */
int sysctl_kcall_nid(const char *const *modules, const char *library, unsigned nid, unsigned arg,
                     int *result, const char **which);

void sysctl_sleep(void);
void sysctl_shutdown(void);
void sysctl_restart(void);

/* Free/total bytes of the storage XLT runs from; 0 on success. */
/* Free and total space as last measured (this boot, or saved from an earlier one).
 * Instant; -1 if never measured or there is no such device. */
int sysctl_storage(const char *dev, uint64_t *free_bytes, uint64_t *total_bytes);
void sysctl_storage_load(const char *app_dir); /* cache/storage.txt, at startup */
/* The real measurement (walks the FAT: ~12 s on a big card, nothing else runs
 * meanwhile). Only when the user asks (Settings > About). Returns the time it took, in ms. */
int sysctl_storage_measure(void);
int sysctl_storage_date(void); /* yyyymmdd of the last measurement, 0 = never */
/* The Memory Stick's free space counted by a thread from its FAT (fatcount.c),
 * with progress, while everything else keeps running. COUNT_FAILED means the raw
 * stick couldn't be read: sysctl_storage_measure is the way left. */
enum { COUNT_IDLE, COUNT_RUNNING, COUNT_DONE, COUNT_FAILED, COUNT_CANCELLED };
int sysctl_storage_count_start(void);
int sysctl_storage_count_state(int *permille); /* COUNT_*, progress 0..1000 */
void sysctl_storage_count_cancel(void);
const char *sysctl_storage_count_how(int *ms); /* the device used and the time taken, for the log */

/* Saves the frame currently on screen as a PNG in <dev>/PICTURE/. 0 on success. */
int sysctl_screenshot(const char *dev, char *name_out, int n);

#endif
