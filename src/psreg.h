/* XLT - the PSP's own settings, in the system registry (flash1).
 *
 * Read and written through the kernel's sceReg functions (found by ARK,
 * called through kubridge, like the other kernel calls in sysctl.c), so a
 * missing function never stops XLT from loading. Every value is read back
 * after a write. */
#ifndef PSREG_H
#define PSREG_H

void psreg_init(void);
int psreg_available(void);

/* 0 on success. */
int psreg_get_int(const char *dir, const char *key, int *value);
int psreg_set_int(const char *dir, const char *key, int value);
int psreg_get_str(const char *dir, const char *key, char *out, int n);

#endif
