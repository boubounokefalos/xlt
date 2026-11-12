/* XLT - reading ZIP archives (stored and deflated, no passwords, no ZIP64).
 * Portable (host-tested); fileops.c runs it on the file job thread. The names
 * inside an archive are not trusted: see zip_safe_path. */
#ifndef ZIP_H
#define ZIP_H

#include <stdint.h>

#define ZIP_PATH 256

enum {
    ZIP_OK = 0,
    ZIP_ERR_READ = -1,
    ZIP_ERR_NOT_ZIP = -2,
    ZIP_ERR_ZIP64 = -3,      /* over 4 GB or 65535 files */
    ZIP_ERR_TOO_BIG = -4,    /* its list of files won't fit in memory */
    ZIP_ERR_NO_MEM = -5,
    ZIP_ERR_METHOD = -6,     /* a compression other than store / deflate */
    ZIP_ERR_ENCRYPTED = -7,
    ZIP_ERR_DATA = -8,       /* damaged, or doesn't match its checksum */
    ZIP_ERR_WRITE = -9,
    ZIP_CANCELLED = -10
};

/* Up to len bytes at off into dst: the count, or < 0. */
typedef int (*ZipReadFn)(void *ctx, uint64_t off, void *dst, uint32_t len);
/* All of len bytes, or < 0. */
typedef int (*ZipWriteFn)(void *ctx, const void *src, uint32_t len);

typedef struct {
    ZipReadFn read;
    void *ctx;
    uint8_t *cd;       /* the central directory */
    uint32_t cd_size, pos;
    uint32_t base;     /* bytes before the archive (a self-extracting stub) */
    uint32_t count, seen;
} ZipDir;

typedef struct {
    char name[ZIP_PATH]; /* as stored: not safe to use as a path */
    int is_dir, encrypted, method;
    uint32_t csize, usize, crc, offset;
} ZipEntry;

int zip_open(ZipDir *z, ZipReadFn read, void *ctx, uint64_t file_size);
void zip_rewind(ZipDir *z);
/* The next entry: 1, 0 at the end, or < 0 if the list is damaged. */
int zip_next(ZipDir *z, ZipEntry *e);
void zip_close(ZipDir *z);

/* Writes one file out. in and out are working buffers. cancel may be NULL. */
int zip_extract(const ZipDir *z, const ZipEntry *e, ZipWriteFn write, void *wctx,
                uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_len, volatile int *cancel);

/* A stored name as a relative path that stays inside the destination: '\' becomes
 * '/', characters FAT can't hold become '_', and so do bytes that aren't UTF-8.
 * 0, or -1 if it can't be made safe (absolute, "..", a drive, or too long). */
int zip_safe_path(const char *raw, char *out, int n);
/* 1 for what other systems leave behind: __MACOSX/, .DS_Store, Thumbs.db, desktop.ini. */
int zip_is_junk(const char *path);

const char *zip_error_text(int err);

/* Where a homebrew in this archive belongs, by the place of its EBOOT.PBP. */
enum {
    ZIP_INSTALL_NONE,
    ZIP_INSTALL_PSP_GAME, /* PSP/GAME/App/EBOOT.PBP: unpacks at the root of the stick */
    ZIP_INSTALL_FOLDER,   /* App/EBOOT.PBP, one top folder: unpacks in PSP/GAME */
    ZIP_INSTALL_BARE      /* EBOOT.PBP at the top: unpacks in a new folder in PSP/GAME */
};

typedef struct {
    int kind;
    char name[64]; /* the App above; "" for BARE */
} ZipPlan;

void zip_install_plan(ZipDir *z, ZipPlan *out);

#endif
