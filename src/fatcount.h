/* XLT - free space by reading the FAT ourselves (pure: no PSP calls).
 *
 * The PSP's own free-space call (sceIoDevctl 0x02425818) walks the whole FAT in
 * the driver and holds the system for ~12 s on a big card. Reading the same
 * table in chunks from a thread costs about as much I/O but lets the UI run and
 * show how far it got. sysctl.c does the reading; this file knows the layout. */
#ifndef XLT_FATCOUNT_H
#define XLT_FATCOUNT_H

#include <stdint.h>

typedef struct {
    int type;              /* 16 or 32 (FAT12 is refused: only on cards under 16 MB) */
    uint32_t sector;       /* bytes per sector */
    uint32_t cluster;      /* bytes per cluster */
    uint32_t clusters;     /* data clusters: entries 2 .. clusters + 1 */
    uint32_t fat_offset;   /* bytes from the start of the partition to the first FAT */
    uint32_t fat_bytes;    /* bytes of the FAT that hold the entries above */
} FatGeom;

/* The first partition's start sector (LBA) from an MBR, or -1 if sec isn't one. */
int64_t fat_mbr_first(const uint8_t *sec512);

/* 0 and the geometry from a FAT16/FAT32 boot sector, -1 if it isn't one. */
int fat_parse_boot(const uint8_t *sec512, FatGeom *g);

/* Free entries in buf, which holds the FAT's bytes from byte offset `at` on
 * (multiples of the entry size). Entries 0, 1 and those past the last cluster
 * aren't counted. */
uint32_t fat_count_free(const FatGeom *g, const uint8_t *buf, uint32_t len, uint32_t at);

#endif
