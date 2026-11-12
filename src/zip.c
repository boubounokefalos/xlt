/* XLT - reading ZIP archives. See zip.h.
 *
 * The central directory at the end of the file is what counts: it has the final
 * sizes and checksums even for entries written with a data descriptor. Every
 * length read from the archive is checked before it is used, and a file can't
 * write more than the size it declares. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>
#include "zip.h"

#define EOCD_SIG 0x06054b50u
#define CEN_SIG 0x02014b50u
#define LOC_SIG 0x04034b50u
#define EOCD_LEN 22
#define CEN_LEN 46
#define LOC_LEN 30
#define MAX_COMMENT 65535
#define MAX_CD (4u * 1024 * 1024)

static uint32_t le16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

int zip_open(ZipDir *z, ZipReadFn read, void *ctx, uint64_t file_size)
{
    memset(z, 0, sizeof(*z));
    z->read = read;
    z->ctx = ctx;
    if (file_size < EOCD_LEN)
        return ZIP_ERR_NOT_ZIP;
    uint32_t tail = file_size < EOCD_LEN + MAX_COMMENT ? (uint32_t)file_size : EOCD_LEN + MAX_COMMENT;
    uint64_t tail_at = file_size - tail;
    uint8_t *b = malloc(tail);
    if (!b)
        return ZIP_ERR_NO_MEM;
    if (read(ctx, tail_at, b, tail) != (int)tail) {
        free(b);
        return ZIP_ERR_READ;
    }
    int at = -1;
    for (int i = (int)tail - EOCD_LEN; i >= 0; i--)
        if (le32(b + i) == EOCD_SIG && (uint32_t)i + EOCD_LEN + le16(b + i + 20) == tail) {
            at = i;
            break;
        }
    if (at < 0) {
        free(b);
        return ZIP_ERR_NOT_ZIP;
    }
    const uint8_t *e = b + at;
    uint32_t count = le16(e + 10), size = le32(e + 12), off = le32(e + 16);
    uint32_t disk = le16(e + 4), start_disk = le16(e + 6);
    uint64_t eocd_at = tail_at + (uint64_t)at;
    free(b);
    if (count == 0xFFFF || size == 0xFFFFFFFFu || off == 0xFFFFFFFFu)
        return ZIP_ERR_ZIP64;
    if (disk || start_disk || (uint64_t)off + size > eocd_at)
        return ZIP_ERR_NOT_ZIP; /* split archives too */
    if (size > MAX_CD)
        return ZIP_ERR_TOO_BIG;
    z->base = (uint32_t)(eocd_at - ((uint64_t)off + size)); /* a stub before the archive shifts everything */
    z->cd = malloc(size ? size : 1);
    if (!z->cd)
        return ZIP_ERR_NO_MEM;
    if (size && read(ctx, (uint64_t)off + z->base, z->cd, size) != (int)size) {
        zip_close(z);
        return ZIP_ERR_READ;
    }
    z->cd_size = size;
    z->count = count;
    return ZIP_OK;
}

void zip_rewind(ZipDir *z) { z->pos = z->seen = 0; }

int zip_next(ZipDir *z, ZipEntry *e)
{
    if (z->seen >= z->count)
        return 0;
    if (z->pos > z->cd_size || z->cd_size - z->pos < CEN_LEN)
        return ZIP_ERR_DATA;
    const uint8_t *c = z->cd + z->pos;
    if (le32(c) != CEN_SIG)
        return ZIP_ERR_DATA;
    uint32_t nlen = le16(c + 28), xlen = le16(c + 30), clen = le16(c + 32);
    uint32_t total = CEN_LEN + nlen + xlen + clen;
    if (z->cd_size - z->pos < total)
        return ZIP_ERR_DATA;
    if (nlen >= sizeof(e->name)) { /* kept short: zip_safe_path refuses it */
        memset(e->name, '/', 2);
        e->name[2] = 0;
    } else {
        memcpy(e->name, c + CEN_LEN, nlen);
        e->name[nlen] = 0;
    }
    uint32_t flags = le16(c + 8);
    e->encrypted = (flags & 1) != 0;
    e->method = (int)le16(c + 10);
    e->crc = le32(c + 16);
    e->csize = le32(c + 20);
    e->usize = le32(c + 24);
    e->offset = le32(c + 42);
    e->is_dir = nlen > 0 && (e->name[nlen - 1] == '/' || e->name[nlen - 1] == '\\');
    z->pos += total;
    z->seen++;
    return 1;
}

void zip_close(ZipDir *z)
{
    free(z->cd);
    z->cd = NULL;
}

/* Where the entry's data starts: past its local header, whose lengths can differ. */
static int data_start(const ZipDir *z, const ZipEntry *e, uint64_t *out)
{
    uint8_t h[LOC_LEN];
    uint64_t at = (uint64_t)e->offset + z->base;
    if (z->read(z->ctx, at, h, LOC_LEN) != LOC_LEN)
        return ZIP_ERR_READ;
    if (le32(h) != LOC_SIG)
        return ZIP_ERR_DATA;
    *out = at + LOC_LEN + le16(h + 26) + le16(h + 28);
    return ZIP_OK;
}

static int extract_stored(const ZipDir *z, const ZipEntry *e, uint64_t at, ZipWriteFn write, void *wctx,
                          uint8_t *buf, uint32_t buf_len, volatile int *cancel)
{
    if (e->csize != e->usize)
        return ZIP_ERR_DATA;
    uint32_t left = e->csize, crc = (uint32_t)crc32(0, NULL, 0);
    while (left) {
        if (cancel && *cancel)
            return ZIP_CANCELLED;
        uint32_t want = left < buf_len ? left : buf_len;
        if (z->read(z->ctx, at, buf, want) != (int)want)
            return ZIP_ERR_DATA;
        crc = (uint32_t)crc32(crc, buf, want);
        if (write(wctx, buf, want) < 0)
            return ZIP_ERR_WRITE;
        at += want;
        left -= want;
    }
    return crc == e->crc ? ZIP_OK : ZIP_ERR_DATA;
}

static int extract_deflated(const ZipDir *z, const ZipEntry *e, uint64_t at, ZipWriteFn write, void *wctx,
                            uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_len, volatile int *cancel)
{
    z_stream s;
    memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, -15) != Z_OK)
        return ZIP_ERR_NO_MEM;
    uint32_t left = e->csize, written = 0, crc = (uint32_t)crc32(0, NULL, 0);
    int r = ZIP_OK, zr = Z_OK;
    while (r == ZIP_OK && zr != Z_STREAM_END) {
        if (cancel && *cancel) {
            r = ZIP_CANCELLED;
            break;
        }
        if (s.avail_in == 0 && left) {
            uint32_t want = left < in_len ? left : in_len;
            if (z->read(z->ctx, at, in, want) != (int)want) {
                r = ZIP_ERR_DATA;
                break;
            }
            at += want;
            left -= want;
            s.next_in = in;
            s.avail_in = want;
        }
        s.next_out = out;
        s.avail_out = out_len;
        zr = inflate(&s, Z_NO_FLUSH);
        if (zr != Z_OK && zr != Z_STREAM_END) { /* Z_BUF_ERROR too: the data ran out early */
            r = ZIP_ERR_DATA;
            break;
        }
        uint32_t got = out_len - s.avail_out;
        if (got > e->usize - written) { /* more than it declared */
            r = ZIP_ERR_DATA;
            break;
        }
        if (got) {
            crc = (uint32_t)crc32(crc, out, got);
            if (write(wctx, out, got) < 0) {
                r = ZIP_ERR_WRITE;
                break;
            }
            written += got;
        }
    }
    inflateEnd(&s);
    if (r == ZIP_OK && (written != e->usize || crc != e->crc))
        r = ZIP_ERR_DATA;
    return r;
}

int zip_extract(const ZipDir *z, const ZipEntry *e, ZipWriteFn write, void *wctx,
                uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_len, volatile int *cancel)
{
    if (e->encrypted)
        return ZIP_ERR_ENCRYPTED;
    if (e->method != 0 && e->method != 8)
        return ZIP_ERR_METHOD;
    uint64_t at;
    int r = data_start(z, e, &at);
    if (r != ZIP_OK)
        return r;
    return e->method == 0 ? extract_stored(z, e, at, write, wctx, in, in_len, cancel)
                          : extract_deflated(z, e, at, write, wctx, in, in_len, out, out_len, cancel);
}

/* How many bytes of valid UTF-8 start at s (0 if none). */
static int utf8_len(const unsigned char *s)
{
    int n = s[0] >= 0xF0 && s[0] <= 0xF4 ? 4 : s[0] >= 0xE0 && s[0] < 0xF0 ? 3 : s[0] >= 0xC2 && s[0] < 0xE0 ? 2 : 0;
    for (int i = 1; i < n; i++)
        if ((s[i] & 0xC0) != 0x80)
            return 0;
    return n;
}

int zip_safe_path(const char *raw, char *out, int n)
{
    if (!raw[0] || raw[0] == '/' || raw[0] == '\\')
        return -1;
    int w = 0, comp = 0; /* comp: where the current part of the path starts in out */
    for (const unsigned char *s = (const unsigned char *)raw;; s++) {
        unsigned c = *s;
        if (c == '\\') c = '/';
        if (c == '/' || c == 0) { /* a part ends */
            int len = w - comp;
            if (len == 2 && out[comp] == '.' && out[comp + 1] == '.')
                return -1;
            if (len == 1 && out[comp] == '.')
                w = comp; /* "." */
            while (w > comp && (out[w - 1] == ' ' || out[w - 1] == '.'))
                w--; /* FAT would drop these and the name would no longer match */
            if (c == 0)
                break;
            if (w > comp) { /* an empty part ("a//b") disappears */
                if (w + 1 >= n) return -1;
                out[w++] = '/';
                comp = w;
            }
            continue;
        }
        if (c < 0x20)
            return -1;
        if (c == ':' && comp == 0)
            return -1; /* "ms0:/x" or "C:\x" */
        int ul = c >= 0x80 ? utf8_len(s) : 1;
        if (ul == 0 || strchr(":*?\"<>|", (int)c)) { /* not UTF-8 (usually CP437), or not allowed on FAT */
            c = '_';
            ul = 1;
        }
        if (w + ul >= n)
            return -1;
        if (ul == 1) {
            out[w++] = (char)c;
        } else {
            memcpy(out + w, s, ul);
            w += ul;
            s += ul - 1;
        }
    }
    if (w && out[w - 1] == '/')
        w--;
    out[w] = 0;
    return w ? 0 : -1;
}

int zip_is_junk(const char *path)
{
    if (!strncasecmp(path, "__MACOSX", 8) && (path[8] == 0 || path[8] == '/'))
        return 1;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return !strcasecmp(base, ".DS_Store") || !strcasecmp(base, "Thumbs.db") || !strcasecmp(base, "desktop.ini");
}

const char *zip_error_text(int err)
{
    switch (err) {
    case ZIP_ERR_READ: return "Couldn't read";
    case ZIP_ERR_NOT_ZIP: return "Not a ZIP file:";
    case ZIP_ERR_ZIP64: return "Too large a ZIP:";
    case ZIP_ERR_TOO_BIG: return "Too many files in";
    case ZIP_ERR_NO_MEM: return "Not enough memory for";
    case ZIP_ERR_METHOD: return "Unknown compression:";
    case ZIP_ERR_ENCRYPTED: return "Password protected:";
    case ZIP_ERR_DATA: return "Damaged:";
    case ZIP_ERR_WRITE: return "No room for";
    default: return "Couldn't extract";
    }
}

/* The n'th part of a path into out; 0 if there is no such part. */
static int part(const char *p, int n, char *out, int cap)
{
    for (; n > 0; n--) {
        p = strchr(p, '/');
        if (!p)
            return 0;
        p++;
    }
    const char *end = strchr(p, '/');
    int len = end ? (int)(end - p) : (int)strlen(p);
    if (len >= cap)
        return 0;
    memcpy(out, p, len);
    out[len] = 0;
    return 1;
}

void zip_install_plan(ZipDir *z, ZipPlan *out)
{
    memset(out, 0, sizeof(*out));
    ZipEntry e;
    char safe[ZIP_PATH], top[64], first_top[64] = "", game[64] = "", folder[64] = "";
    int bare = 0, in_folder = 0, in_game = 0, other = 0, tops = 0, all_psp = 1, r;
    zip_rewind(z);
    while ((r = zip_next(z, &e)) > 0) {
        if (zip_safe_path(e.name, safe, sizeof(safe)) != 0) {
            zip_rewind(z);
            return; /* an archive like this gets no shortcut */
        }
        if (zip_is_junk(safe))
            continue;
        if (!part(safe, 0, top, sizeof(top))) {
            zip_rewind(z);
            return;
        }
        if (!first_top[0]) {
            memcpy(first_top, top, sizeof(top));
            tops = 1;
        } else if (strcasecmp(first_top, top)) {
            tops++;
        }
        if (strcasecmp(top, "PSP"))
            all_psp = 0;
        const char *base = strrchr(safe, '/');
        base = base ? base + 1 : safe;
        if (e.is_dir || strcasecmp(base, "EBOOT.PBP"))
            continue;
        int depth = 0;
        for (const char *p = safe; *p; p++)
            depth += *p == '/';
        char a[16], b[16];
        if (depth == 0) {
            bare = 1;
        } else if (depth == 1) {
            in_folder = 1;
            part(safe, 0, folder, sizeof(folder));
        } else if (depth == 3 && part(safe, 0, a, sizeof(a)) && part(safe, 1, b, sizeof(b)) &&
                   !strcasecmp(a, "PSP") && !strcasecmp(b, "GAME") && part(safe, 2, game[0] ? top : game, sizeof(game))) {
            in_game = 1;
        } else {
            other = 1;
        }
    }
    zip_rewind(z);
    if (r < 0 || other || bare + in_folder + in_game != 1)
        return;
    if (in_game && all_psp) {
        out->kind = ZIP_INSTALL_PSP_GAME;
        snprintf(out->name, sizeof(out->name), "%s", game);
    } else if (in_folder && tops == 1) {
        out->kind = ZIP_INSTALL_FOLDER;
        snprintf(out->name, sizeof(out->name), "%s", folder);
    } else if (bare) {
        out->kind = ZIP_INSTALL_BARE;
    }
}
