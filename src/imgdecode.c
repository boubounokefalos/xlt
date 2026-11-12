/* XLT - streaming picture decoder. See imgdecode.h. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <jpeglib.h>
#include <jerror.h>
#include <png.h>
#include <gif_lib.h>
#include "imgdecode.h"

#define MAX_SIDE 8192
#define MAX_INTERLACED (4 * 1024 * 1024) /* interlaced PNGs need the whole image */

/* --------------------------------------------------------------- sampling */

typedef struct {
    int dw, dh;          /* output size */
    int cx, cy, cw, ch;  /* source rectangle (in decoded-image pixels) */
    int next_y;          /* next output row to fill */
    uint8_t *out;
    int stride;          /* output row bytes */
} Sampler;

/* Sets up crop and output size for a decoded image of sw x sh. */
static void sampler_init(Sampler *s, int sw, int sh, int mode, int ow, int oh, uint8_t *out, int *rw, int *rh)
{
    memset(s, 0, sizeof(*s));
    s->out = out;
    s->stride = ow * 4;
    if (mode == IMG_COVER) {
        s->dw = ow;
        s->dh = oh;
        /* crop the source to the output's aspect, centred */
        if ((long long)sw * oh > (long long)sh * ow) {
            s->ch = sh;
            s->cw = (int)((long long)sh * ow / oh);
        } else {
            s->cw = sw;
            s->ch = (int)((long long)sw * oh / ow);
        }
        if (s->cw < 1) s->cw = 1;
        if (s->ch < 1) s->ch = 1;
        s->cx = (sw - s->cw) / 2;
        s->cy = (sh - s->ch) / 2;
    } else {
        s->cw = sw;
        s->ch = sh;
        /* shrink to fit, never enlarge */
        if ((long long)sw * oh > (long long)sh * ow) {
            s->dw = sw < ow ? sw : ow;
            s->dh = (int)((long long)sh * s->dw / sw);
        } else {
            s->dh = sh < oh ? sh : oh;
            s->dw = (int)((long long)sw * s->dh / sh);
        }
        if (s->dw < 1) s->dw = 1;
        if (s->dh < 1) s->dh = 1;
    }
    *rw = s->dw;
    *rh = s->dh;
}

/* Source row (0-based, in decoded pixels) needed by output row y. */
static int src_row(const Sampler *s, int y)
{
    return s->cy + (int)((long long)y * s->ch / s->dh);
}

/* Feeds decoded row r. fmt: 3 = RGB, 4 = RGBA, -3 = BGR (BMP), -4 = BGRA. */
static void sampler_row(Sampler *s, int r, const uint8_t *row, int fmt)
{
    int bpp = fmt < 0 ? -fmt : fmt, bgr = fmt < 0;
    while (s->next_y < s->dh && src_row(s, s->next_y) == r) {
        uint8_t *o = s->out + (size_t)s->next_y * s->stride;
        for (int x = 0; x < s->dw; x++) {
            const uint8_t *p = row + (size_t)(s->cx + (int)((long long)x * s->cw / s->dw)) * bpp;
            o[0] = bgr ? p[2] : p[0];
            o[1] = p[1];
            o[2] = bgr ? p[0] : p[2];
            o[3] = bpp == 4 ? p[3] : 255;
            o += 4;
        }
        s->next_y++;
    }
}

static int sampler_wants(const Sampler *s, int r)
{
    return s->next_y < s->dh && src_row(s, s->next_y) <= r;
}

/* ------------------------------------------------------------------ JPEG */

typedef struct {
    struct jpeg_error_mgr mgr;
    jmp_buf jmp;
    int truncated; /* the file ended early: libjpeg only warns and pads with grey */
} JpegErr;

static void jpeg_fail(j_common_ptr c)
{
    longjmp(((JpegErr *)c->err)->jmp, 1);
}

/* Warnings are ignored (camera files often have a few stray bytes), except a
 * premature end of file, which would otherwise give a half-grey picture. */
static void jpeg_quiet(j_common_ptr c, int level)
{
    if (level < 0 && c->err->msg_code == JWRN_JPEG_EOF)
        ((JpegErr *)c->err)->truncated = 1;
}

static int decode_jpeg(FILE *f, int mode, int ow, int oh, uint8_t *out, int *rw, int *rh)
{
    struct jpeg_decompress_struct cinfo;
    JpegErr err;
    uint8_t *volatile row = NULL;
    cinfo.err = jpeg_std_error(&err.mgr);
    err.truncated = 0;
    err.mgr.error_exit = jpeg_fail;
    err.mgr.emit_message = jpeg_quiet;
    if (setjmp(err.jmp)) {
        jpeg_destroy_decompress(&cinfo);
        free(row);
        return -1;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);
    if (cinfo.image_width > MAX_SIDE || cinfo.image_height > MAX_SIDE)
        longjmp(err.jmp, 1);
    cinfo.out_color_space = JCS_RGB;

    /* Let libjpeg shrink by up to 1/8 while keeping at least the target size. */
    Sampler probe;
    int tw, th;
    sampler_init(&probe, cinfo.image_width, cinfo.image_height, mode, ow, oh, out, &tw, &th);
    int denom = 8;
    while (denom > 1 && (probe.cw / denom < tw || probe.ch / denom < th))
        denom /= 2;
    cinfo.scale_num = 1;
    cinfo.scale_denom = denom;
    jpeg_start_decompress(&cinfo);

    Sampler s;
    sampler_init(&s, cinfo.output_width, cinfo.output_height, mode, ow, oh, out, rw, rh);
    row = malloc((size_t)cinfo.output_width * cinfo.output_components);
    if (!row)
        longjmp(err.jmp, 1);
    while (cinfo.output_scanline < cinfo.output_height && s.next_y < s.dh) {
        int r = cinfo.output_scanline;
        JSAMPROW rows[1] = {row};
        jpeg_read_scanlines(&cinfo, rows, 1);
        if (sampler_wants(&s, r))
            sampler_row(&s, r, row, 3); /* out_color_space is RGB */
    }
    jpeg_abort_decompress(&cinfo); /* we may stop early: skip the rest */
    jpeg_destroy_decompress(&cinfo);
    free(row);
    return s.next_y == s.dh && !err.truncated ? 0 : -1;
}

/* ------------------------------------------------------------------- PNG */

static void png_quiet(png_structp p, png_const_charp m) { (void)p; (void)m; }

static int decode_png(FILE *f, int mode, int ow, int oh, uint8_t *out, int *rw, int *rh)
{
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, png_quiet);
    if (!png)
        return -1;
    png_infop info = png_create_info_struct(png);
    uint8_t *volatile buf = NULL;
    if (!info || setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, info ? &info : NULL, NULL);
        free(buf);
        return -1;
    }
    png_init_io(png, f);
    png_read_info(png, info);
    png_uint_32 w = png_get_image_width(png, info), h = png_get_image_height(png, info);
    if (w == 0 || h == 0 || w > MAX_SIDE || h > MAX_SIDE)
        png_error(png, "size");
    /* everything to 8-bit RGBA */
    png_set_expand(png);
    png_set_strip_16(png);
    png_set_gray_to_rgb(png);
    png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    int passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);

    Sampler s;
    sampler_init(&s, w, h, mode, ow, oh, out, rw, rh);
    if (passes > 1) {
        /* interlaced: rows are final only after the last pass */
        if ((size_t)w * h * 4 > MAX_INTERLACED)
            png_error(png, "interlaced and too large");
        buf = malloc((size_t)w * h * 4);
        if (!buf)
            png_error(png, "memory");
        png_bytep *rows = malloc(sizeof(png_bytep) * h);
        if (!rows)
            png_error(png, "memory");
        for (png_uint_32 y = 0; y < h; y++)
            rows[y] = buf + (size_t)y * w * 4;
        png_read_image(png, rows);
        free(rows);
        for (png_uint_32 y = 0; y < h; y++)
            if (sampler_wants(&s, y))
                sampler_row(&s, y, buf + (size_t)y * w * 4, 4);
    } else {
        buf = malloc((size_t)w * 4);
        if (!buf)
            png_error(png, "memory");
        for (png_uint_32 y = 0; y < h && s.next_y < s.dh; y++) {
            png_read_row(png, buf, NULL);
            if (sampler_wants(&s, y))
                sampler_row(&s, y, buf, 4);
        }
    }
    png_destroy_read_struct(&png, &info, NULL);
    free(buf);
    return s.next_y == s.dh ? 0 : -1;
}

/* ------------------------------------------------------------------- BMP */

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Uncompressed 24- and 32-bit BMPs (what Capture writes). */
static int decode_bmp(FILE *f, int mode, int ow, int oh, uint8_t *out, int *rw, int *rh)
{
    uint8_t h[54];
    if (fread(h, 1, 54, f) != 54 || h[0] != 'B' || h[1] != 'M')
        return -1;
    uint32_t data = rd32(h + 10), comp = rd32(h + 30);
    int32_t w = (int32_t)rd32(h + 18), ht = (int32_t)rd32(h + 22);
    int bpp = h[28] | h[29] << 8;
    int top_down = ht < 0;
    if (top_down) ht = -ht;
    if ((bpp != 24 && bpp != 32) || (comp != 0 && comp != 3) || w <= 0 || ht <= 0 || w > MAX_SIDE || ht > MAX_SIDE)
        return -1;
    int bytes = bpp / 8;
    long stride = ((long)w * bytes + 3) & ~3L;
    uint8_t *row = malloc(stride);
    if (!row)
        return -1;
    Sampler s;
    sampler_init(&s, w, ht, mode, ow, oh, out, rw, rh);
    int ok = 1;
    for (int y = 0; y < s.dh && ok; y++) { /* seek to just the rows we need */
        int r = src_row(&s, y);
        long file_row = top_down ? r : ht - 1 - r;
        ok = fseek(f, (long)data + file_row * stride, SEEK_SET) == 0 && fread(row, 1, stride, f) == (size_t)stride;
        if (ok)
            sampler_row(&s, r, row, bytes == 3 ? -3 : -4);
    }
    free(row);
    return ok && s.next_y == s.dh ? 0 : -1;
}

/* ------------------------------------------------------------------- GIF */

typedef struct { const uint8_t *p; int len, pos; } MemSrc;

static int gif_read(GifFileType *g, GifByteType *buf, int n)
{
    MemSrc *m = g->UserData;
    if (n > m->len - m->pos) n = m->len - m->pos;
    if (n < 0) n = 0;
    memcpy(buf, m->p + m->pos, n);
    m->pos += n;
    return n;
}

#define GIF_MAX_CANVAS (2 * 1024 * 1024) /* pixels */

/* The first frame, on a transparent canvas of the GIF's screen size. */
static int decode_gif(const uint8_t *buf, int len, int mode, int ow, int oh, uint8_t *out, int *rw, int *rh)
{
    MemSrc m = {buf, len, 0};
    int err = 0;
    GifFileType *g = DGifOpen(&m, gif_read, &err);
    if (!g)
        return -1;
    int r = -1, transparent = -1;
    uint8_t *canvas = NULL, *line = NULL;
    int W = g->SWidth, H = g->SHeight;
    if (W <= 0 || H <= 0 || W > MAX_SIDE || H > MAX_SIDE || (long)W * H > GIF_MAX_CANVAS)
        goto done;
    canvas = calloc((size_t)W * H, 4);
    line = malloc((size_t)W);
    if (!canvas || !line)
        goto done;
    for (;;) {
        GifRecordType rt;
        if (DGifGetRecordType(g, &rt) == GIF_ERROR)
            goto done;
        if (rt == TERMINATE_RECORD_TYPE)
            goto done;
        if (rt == EXTENSION_RECORD_TYPE) {
            int code;
            GifByteType *ext;
            if (DGifGetExtension(g, &code, &ext) == GIF_ERROR)
                goto done;
            if (code == GRAPHICS_EXT_FUNC_CODE && ext && ext[0] >= 4 && (ext[1] & 1))
                transparent = ext[4];
            while (ext) {
                if (DGifGetExtensionNext(g, &ext) == GIF_ERROR)
                    goto done;
            }
            continue;
        }
        if (rt != IMAGE_DESC_RECORD_TYPE)
            continue;
        if (DGifGetImageDesc(g) == GIF_ERROR)
            goto done;
        GifImageDesc *d = &g->Image;
        ColorMapObject *cm = d->ColorMap ? d->ColorMap : g->SColorMap;
        if (!cm || d->Width <= 0 || d->Height <= 0 || d->Width > W * 2 || d->Height > H * 2)
            goto done;
        static const int ioff[4] = {0, 4, 2, 1}, istep[4] = {8, 8, 4, 2};
        int passes = d->Interlace ? 4 : 1;
        uint8_t *row = malloc((size_t)d->Width);
        if (!row)
            goto done;
        int ok = 1;
        for (int pass = 0; pass < passes && ok; pass++) {
            int y0 = d->Interlace ? ioff[pass] : 0, step = d->Interlace ? istep[pass] : 1;
            for (int y = y0; y < d->Height; y += step) {
                if (DGifGetLine(g, row, d->Width) == GIF_ERROR) { ok = 0; break; }
                int cy = d->Top + y;
                if (cy < 0 || cy >= H) continue;
                for (int x = 0; x < d->Width; x++) {
                    int cx = d->Left + x;
                    if (cx < 0 || cx >= W || row[x] == transparent || row[x] >= cm->ColorCount) continue;
                    GifColorType c = cm->Colors[row[x]];
                    uint8_t *o = canvas + ((size_t)cy * W + cx) * 4;
                    o[0] = c.Red; o[1] = c.Green; o[2] = c.Blue; o[3] = 255;
                }
            }
        }
        free(row);
        if (!ok)
            goto done;
        Sampler s;
        sampler_init(&s, W, H, mode, ow, oh, out, rw, rh);
        for (int y = 0; y < H && s.next_y < s.dh; y++)
            if (sampler_wants(&s, y))
                sampler_row(&s, y, canvas + (size_t)y * W * 4, 4);
        r = s.next_y == s.dh ? 0 : -1;
        goto done;
    }
done:
    free(canvas);
    free(line);
    DGifCloseFile(g, &err);
    return r;
}

/* ------------------------------------------------------------------ entry */

static const char *ext(const char *path)
{
    const char *d = strrchr(path, '.');
    return d ? d + 1 : "";
}

int img_is_picture(const char *name)
{
    const char *e = ext(name);
    return !strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg") || !strcasecmp(e, "png") || !strcasecmp(e, "bmp");
}

const char *img_format_name(const char *path)
{
    const char *e = ext(path);
    if (!strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg")) return "JPEG";
    if (!strcasecmp(e, "png")) return "PNG";
    if (!strcasecmp(e, "bmp")) return "BMP";
    return "Picture";
}

int img_decode(const char *path, int mode, int ow, int oh, uint8_t *out, int *rw, int *rh)
{
    return img_decode_at(path, 0, mode, ow, oh, out, rw, rh);
}

int img_decode_at(const char *path, uint32_t offset, int mode, int ow, int oh, uint8_t *out, int *rw, int *rh)
{
    *rw = *rh = 0;
    if (ow <= 0 || oh <= 0)
        return -1;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    uint8_t sig[8] = {0};
    size_t n = 0;
    if (fseek(f, (long)offset, SEEK_SET) == 0)
        n = fread(sig, 1, 8, f);
    fseek(f, (long)offset, SEEK_SET); /* JPEG and PNG read on from here */
    int r = -1;
    if (n >= 3 && sig[0] == 0xFF && sig[1] == 0xD8 && sig[2] == 0xFF)
        r = decode_jpeg(f, mode, ow, oh, out, rw, rh);
    else if (n == 8 && !memcmp(sig, "\x89PNG\r\n\x1a\n", 8))
        r = decode_png(f, mode, ow, oh, out, rw, rh);
    else if (offset == 0 && n >= 2 && sig[0] == 'B' && sig[1] == 'M') /* BMP seeks by absolute offsets */
        r = decode_bmp(f, mode, ow, oh, out, rw, rh);
    fclose(f);
    return r;
}

int img_decode_mem(const uint8_t *buf, int len, int mode, int ow, int oh, uint8_t *out, int *rw, int *rh)
{
    *rw = *rh = 0;
    if (ow <= 0 || oh <= 0 || !buf || len < 8)
        return -1;
    if (!memcmp(buf, "GIF8", 4))
        return decode_gif(buf, len, mode, ow, oh, out, rw, rh);
    int jpeg = buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF;
    int png = !memcmp(buf, "\x89PNG\r\n\x1a\n", 8);
    if (!jpeg && !png)
        return -1;
    FILE *f = fmemopen((void *)buf, len, "rb");
    if (!f)
        return -1;
    int r = jpeg ? decode_jpeg(f, mode, ow, oh, out, rw, rh) : decode_png(f, mode, ow, oh, out, rw, rh);
    fclose(f);
    return r;
}
