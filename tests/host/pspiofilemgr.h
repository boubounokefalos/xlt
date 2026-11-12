#ifndef HOST_PSPIOFILEMGR_H
#define HOST_PSPIOFILEMGR_H
/* Host stand-ins (tests/web_preview.c): there is no Memory Stick, nothing is read or kept. */
typedef int SceUID;
#define PSP_O_RDONLY 1
#define PSP_O_WRONLY 2
#define PSP_O_CREAT 0x200
#define PSP_O_TRUNC 0x400
#define PSP_O_APPEND 0x100
static inline SceUID sceIoOpen(const char *p, int f, int m) { (void)p; (void)f; (void)m; return -1; }
static inline int sceIoRead(SceUID fd, void *b, unsigned n) { (void)fd; (void)b; (void)n; return -1; }
static inline int sceIoWrite(SceUID fd, const void *b, unsigned n) { (void)fd; (void)b; (void)n; return -1; }
static inline int sceIoClose(SceUID fd) { (void)fd; return 0; }
#define PSP_SEEK_SET 0
#define PSP_SEEK_END 2
static inline int sceIoLseek32(SceUID fd, int off, int whence) { (void)fd; (void)off; (void)whence; return -1; }
typedef struct { unsigned short year, month, day, hour, minute, second; unsigned microsecond; } ScePspDateTime;
typedef struct { int st_mode; long long st_size; ScePspDateTime sce_st_mtime; } SceIoStat;
typedef struct { SceIoStat d_stat; char d_name[256]; } SceIoDirent;
#define FIO_S_ISDIR(m) (((m) & 0xF000) == 0x1000)
static inline SceUID sceIoDopen(const char *p) { (void)p; return -1; }
static inline int sceIoDread(SceUID d, SceIoDirent *e) { (void)d; (void)e; return 0; }
static inline int sceIoDclose(SceUID d) { (void)d; return 0; }
static inline int sceIoRename(const char *a, const char *b) { (void)a; (void)b; return -1; }
static inline int sceIoRemove(const char *p) { (void)p; return -1; }
static inline int sceIoGetstat(const char *p, SceIoStat *st) { (void)p; (void)st; return -1; }
static inline int sceIoMkdir(const char *p, int m) { (void)p; (void)m; return 0; }
#endif
