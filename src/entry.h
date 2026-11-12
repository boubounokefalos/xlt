/* XLT - one library entry. Stored as-is in the binary index cache. */
#ifndef ENTRY_H
#define ENTRY_H

#include <stdint.h>

enum {
    KIND_ISO,      /* plain disc image */
    KIND_CSO,
    KIND_ZSO,
    KIND_PSN,      /* EBOOT.PBP, CATEGORY "EG" */
    KIND_PS1,      /* EBOOT.PBP, CATEGORY "ME" */
    KIND_HOMEBREW, /* EBOOT.PBP, CATEGORY "MG" */
    KIND_PICTURE,  /* JPEG / PNG / BMP in PICTURE/ (icon_size != 0 means "has a thumbnail") */
    KIND_MUSIC,    /* a song in MUSIC/; icon_* = embedded cover, or ICON_FOLDER_ART */
    KIND_VIDEO,    /* MP4 / PMF in VIDEO/; id = "480x272", sysver = length "12:34" (display strings) */
    KIND_UMD,      /* the disc in the UMD drive (umd.c); never in the library index */
};

typedef struct {
    char path[200];
    char title[128];
    char id[12];      /* DISC_ID, e.g. ULUS10512 */
    char sysver[8];   /* PSP_SYSTEM_VER, e.g. 6.60 */
    uint64_t size;    /* file size in bytes */
    uint32_t mtime;   /* packed modification time, for change detection */
    uint32_t icon_off; /* ICON0.PNG byte offset in the (uncompressed) container, 0 if none */
    uint32_t icon_size;
    uint32_t key;     /* hash of path + size + mtime: names the thumbnail cache file */
    uint8_t kind;
    uint8_t pad;
    uint16_t track;   /* music: track number, 0 if unknown */
    char artist[48];  /* music (UTF-8) */
    char album[48];
} Entry;

/* icon_off value for music: the cover is cover.jpg/folder.jpg next to the file. */
#define ICON_FOLDER_ART 0xFFFFFFFFu

#define INDEX_MAGIC 0x31584C58u /* "XLX1" */
#define INDEX_VERSION 2 /* 2: music fields (an old index is ignored: one full rescan) */

typedef struct {
    uint32_t magic, version, count, entry_size;
} IndexHeader;

/* Fills title/id/sysver/kind/icon from the file at e->path (size/mtime set by caller).
 * Returns 0 if it is a launchable entry. */
int probe_entry(Entry *e);

uint32_t entry_key(const Entry *e);

/* PARAM.SFO bytes into title / id / sysver; category gets e.g. "UG", "MG" (4 bytes). 0 if it has a title. */
int sfo_parse(const uint8_t *b, uint32_t n, Entry *e, char *category);

static inline int kind_is_homebrew(int k) { return k == KIND_HOMEBREW; }
static inline int kind_is_disc(int k) { return k <= KIND_ZSO; }
static inline int kind_is_picture(int k) { return k == KIND_PICTURE; }
static inline int kind_is_music(int k) { return k == KIND_MUSIC; }
static inline int kind_is_video(int k) { return k == KIND_VIDEO; }
static inline int kind_is_umd(int k) { return k == KIND_UMD; }

#endif
