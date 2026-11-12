#ifndef HOST_PSPKERNEL_H
#define HOST_PSPKERNEL_H
#include "psptypes.h"
static inline void sceKernelDcacheWritebackRange(const void *p, unsigned n) { (void)p; (void)n; }
/* Threads (tests/sd_preview.c): none are started on the PC. */
typedef unsigned int SceSize;
typedef unsigned int SceUInt;
#define PSP_THREAD_ATTR_USER 0x80000000
static inline int sceKernelCreateThread(const char *n, int (*f)(SceSize, void *), int p, int st, unsigned a, void *o)
{ (void)n; (void)f; (void)p; (void)st; (void)a; (void)o; return -1; }
static inline int sceKernelStartThread(int t, SceSize n, void *a) { (void)t; (void)n; (void)a; return -1; }
static inline int sceKernelWaitThreadEnd(int t, SceUInt *to) { (void)t; (void)to; return 0; }
static inline int sceKernelDeleteThread(int t) { (void)t; return 0; }
unsigned int sceKernelGetSystemTimeLow(void); /* a preview that needs a clock gives one */
#endif
