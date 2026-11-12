/* XLT - read PARAM.SFO and locate ICON0 in disc images and EBOOT.PBP files. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "entry.h"
#include "disc.h"
#include "xio.h"

#define SFO_MAX (64 * 1024)

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* Copies a UTF-8 title, dropping ™ ® © and turning newlines into spaces. */
static void copy_title(char *dst, int cap, const char *src, int len)
{
    int o = 0;
    for (int i = 0; i < len && src[i] && o < cap - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == 0xE2 && i + 2 < len && (unsigned char)src[i + 1] == 0x84 &&
            (unsigned char)src[i + 2] == 0xA2) { i += 2; continue; }      /* ™ */
        if (c == 0xC2 && i + 1 < len &&
            ((unsigned char)src[i + 1] == 0xAE || (unsigned char)src[i + 1] == 0xA9)) { i++; continue; }
        if (c == '\n' || c == '\r') c = ' ';
        if (c == ' ' && (o == 0 || dst[o - 1] == ' ')) continue;
        dst[o++] = (char)c;
    }
    while (o > 0 && dst[o - 1] == ' ') o--;
    dst[o] = 0;
}

typedef struct { char category[4]; } SfoExtra;

static int parse_sfo(const uint8_t *b, uint32_t n, Entry *e, SfoExtra *x)
{
    if (n < 20 || memcmp(b, "\0PSF", 4))
        return -1;
    uint32_t keys = rd32(b + 8), data = rd32(b + 12), count = rd32(b + 16);
    if (keys >= n || data >= n || 20 + count * 16 > n)
        return -1;
    memset(x, 0, sizeof(*x));
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *en = b + 20 + i * 16;
        uint32_t ko = keys + rd16(en), len = rd32(en + 4), dof = data + rd32(en + 12);
        if (ko >= n || dof >= n || dof + len > n)
            continue;
        const char *k = (const char *)b + ko, *v = (const char *)b + dof;
        if (memchr(k, 0, n - ko) == NULL)
            continue;
        if (!strcmp(k, "TITLE")) copy_title(e->title, sizeof(e->title), v, (int)len);
        else if (!strcmp(k, "DISC_ID")) copy_title(e->id, sizeof(e->id), v, (int)len);
        else if (!strcmp(k, "PSP_SYSTEM_VER")) copy_title(e->sysver, sizeof(e->sysver), v, (int)len);
        else if (!strcmp(k, "CATEGORY")) copy_title(x->category, sizeof(x->category), v, (int)len);
    }
    return e->title[0] ? 0 : -1;
}

int sfo_parse(const uint8_t *b, uint32_t n, Entry *e, char *category)
{
    SfoExtra x;
    memset(&x, 0, sizeof(x)); /* parse_sfo may give up before it clears it */
    int r = parse_sfo(b, n, e, &x);
    if (category)
        memcpy(category, x.category, sizeof(x.category));
    return r;
}

static int probe_disc(Entry *e)
{
    Disc d;
    if (disc_open(&d, e->path))
        return -1;
    e->kind = d.format == DISC_CSO ? KIND_CSO : d.format == DISC_ZSO ? KIND_ZSO : KIND_ISO;
    uint32_t off, size;
    int r = -1;
    if (!disc_find(&d, "PSP_GAME/PARAM.SFO", &off, &size) && size <= SFO_MAX) {
        uint8_t *b = malloc(size);
        SfoExtra x;
        if (b && !disc_read(&d, b, size, off))
            r = parse_sfo(b, size, e, &x);
        free(b);
    }
    if (!r && !disc_find(&d, "PSP_GAME/ICON0.PNG", &off, &size)) {
        e->icon_off = off;
        e->icon_size = size;
    }
    disc_close(&d);
    return r;
}

static int probe_pbp(Entry *e)
{
    int fd = xio_open(e->path);
    if (fd < 0)
        return -1;
    uint8_t h[40];
    int r = -1;
    if (xio_read_at(fd, h, 40, 0) == 40 && !memcmp(h, "\0PBP", 4)) {
        uint32_t sfo = rd32(h + 8), icon = rd32(h + 12), icon1 = rd32(h + 16);
        if (icon > sfo && icon - sfo <= SFO_MAX) {
            uint32_t n = icon - sfo;
            uint8_t *b = malloc(n);
            SfoExtra x;
            if (b && xio_read_at(fd, b, n, sfo) == (int)n && !parse_sfo(b, n, e, &x)) {
                if (!strcmp(x.category, "MG")) { e->kind = KIND_HOMEBREW; r = 0; }
                else if (!strcmp(x.category, "EG")) { e->kind = KIND_PSN; r = 0; }
                else if (!strcmp(x.category, "ME")) { e->kind = KIND_PS1; r = 0; }
                /* "PG" is a game update, "UG" a UMD stub: not launchable on their own. */
            }
            free(b);
        }
        if (!r && icon1 > icon) {
            e->icon_off = icon;
            e->icon_size = icon1 - icon;
        }
    }
    xio_close(fd);
    return r;
}

int probe_entry(Entry *e)
{
    e->title[0] = e->id[0] = e->sysver[0] = 0;
    e->icon_off = e->icon_size = 0;
    const char *dot = strrchr(e->path, '.');
    int r = dot && !strcasecmp(dot, ".pbp") ? probe_pbp(e) : probe_disc(e);
    e->key = entry_key(e);
    return r;
}

uint32_t entry_key(const Entry *e)
{
    uint32_t h = 2166136261u;
    for (const char *p = e->path; *p; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    uint32_t extra[3] = {(uint32_t)e->size, (uint32_t)(e->size >> 32), e->mtime};
    for (int i = 0; i < 3; i++)
        for (int b = 0; b < 32; b += 8)
            h = (h ^ ((extra[i] >> b) & 0xFF)) * 16777619u;
    return h;
}
