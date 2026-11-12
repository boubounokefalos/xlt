/* XLT - free space by reading the FAT ourselves. See fatcount.h. */
#include "fatcount.h"

static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int64_t fat_mbr_first(const uint8_t *s)
{
    if (s[510] != 0x55 || s[511] != 0xAA)
        return -1;
    /* A boot sector also ends in 55 AA: an MBR's first entry has a status of 0 or 0x80 and a type */
    const uint8_t *e = s + 446;
    if ((e[0] != 0 && e[0] != 0x80) || e[4] == 0)
        return -1;
    uint32_t lba = le32(e + 8);
    return lba ? (int64_t)lba : -1;
}

int fat_parse_boot(const uint8_t *s, FatGeom *g)
{
    if (s[510] != 0x55 || s[511] != 0xAA)
        return -1;
    uint32_t bps = le16(s + 11), spc = s[13], reserved = le16(s + 14), nfats = s[16];
    uint32_t root_entries = le16(s + 17), total = le16(s + 19), fatsz = le16(s + 22);
    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || !spc || (spc & (spc - 1)) || !reserved || !nfats)
        return -1;
    if (!total) total = le32(s + 32);
    if (!fatsz) fatsz = le32(s + 36);
    if (!total || !fatsz)
        return -1;
    uint32_t root_secs = (root_entries * 32 + bps - 1) / bps;
    uint32_t meta = reserved + nfats * fatsz + root_secs;
    if (meta >= total)
        return -1;
    uint32_t clusters = (total - meta) / spc;
    /* The type comes from the cluster count alone (Microsoft's rule), not the label */
    int type = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    if (type == 12)
        return -1;
    uint32_t need = (clusters + 2) * (type / 8);
    if ((uint64_t)fatsz * bps < need)
        return -1;
    g->type = type;
    g->sector = bps;
    g->cluster = bps * spc;
    g->clusters = clusters;
    g->fat_offset = reserved * bps;
    g->fat_bytes = need;
    return 0;
}

uint32_t fat_count_free(const FatGeom *g, const uint8_t *buf, uint32_t len, uint32_t at)
{
    uint32_t es = g->type / 8, n = 0;
    uint32_t i = at / es, end = (at + len) / es, last = g->clusters + 2;
    if (end > last) end = last;
    if (i < 2) i = 2;
    const uint8_t *p = buf + (i * es - at);
    if (g->type == 32) {
        for (; i < end; i++, p += 4)
            n += (le32(p) & 0x0FFFFFFF) == 0;
    } else {
        for (; i < end; i++, p += 2)
            n += le16(p) == 0;
    }
    return n;
}
