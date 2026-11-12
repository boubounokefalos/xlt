/* XLT - PDF text extraction. See pdf.h.
 *
 * Objects are parsed into an arena that is emptied before each page (and while
 * walking the page tree), so memory stays bounded however big the file is.
 * Content-stream operands are released after every operator; objects loaded
 * while running an operator stay until the page is done (the object cache
 * points at them).
 *
 * Supported: xref tables and xref streams (with /Prev chains and hybrid
 * files), object streams, FlateDecode (with PNG predictors), LZWDecode,
 * ASCIIHex, ASCII85 and RunLength; simple fonts (standard encodings and
 * /Differences), composite fonts through /ToUnicode, Form XObjects. A file
 * whose xref is broken is rescanned for "N G obj" headers.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "pdf.h"
#include "textenc.h"
#include "xio.h"

#define MAX_OBJS 262144
#define ARENA_SIZE (2u << 20)
#define MAX_STREAM (8 << 20)
#define MAX_PAGES 20000
#define OBJSTM_CACHE 4
#define MAX_FONTS 48
#define MAX_OPS 32

enum { O_NULL, O_BOOL, O_NUM, O_STR, O_NAME, O_ARR, O_DICT, O_REF, O_KW };

typedef struct Obj {
    uint8_t t, stream, isint;
    int n;   /* bytes (string, name, keyword), items (array), pairs (dict), number (ref), value (bool) */
    float num;
    union {
        char *s;
        struct Obj *a; /* array items; dict: key, value, key, value... */
    } u;
    uint32_t soff; /* stream: file offset of the data */
} Obj;

enum { X_UNSET, X_OFF, X_STM, X_FREE };
typedef struct {
    uint32_t off; /* X_OFF: file offset; X_STM: object stream number */
    uint16_t idx;
    uint8_t type;
} XEnt;

typedef struct {
    int num;
    uint8_t *data;
    int len, first, n;
    int *nums, *offs;
    uint32_t stamp;
} ObjStm;

struct PdfDoc {
    int fd;
    uint32_t size;
    XEnt *x;
    int nx;
    int root;
    int encrypted;
    int *pages, *res_owner;
    int npages;
    uint8_t *visited;
    char *ar;
    size_t ar_used;
    struct { int num; Obj *o; } cache[512];
    ObjStm stm[OBJSTM_CACHE];
    uint32_t stamp;
    int depth;
};

/* ------------------------------------------------------------------ arena */

static void *aalloc(PdfDoc *d, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    if (d->ar_used + n > ARENA_SIZE)
        return NULL;
    void *p = d->ar + d->ar_used;
    d->ar_used += n;
    return p;
}

static void areset(PdfDoc *d)
{
    d->ar_used = 0;
    memset(d->cache, 0, sizeof(d->cache));
}

static int rd(PdfDoc *d, uint32_t off, void *buf, int n)
{
    if (off >= d->size || n <= 0)
        return 0;
    if ((uint64_t)off + (uint64_t)n > d->size)
        n = (int)(d->size - off);
    int r = xio_read_at(d->fd, buf, (uint32_t)n, off);
    return r < 0 ? 0 : r;
}

/* ------------------------------------------------------------------ lexer */

typedef struct {
    const uint8_t *p, *end;
    PdfDoc *d;
    int trunc; /* ran into the end of the buffer mid-value */
} Lex;

static int is_ws(int c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
static int is_delim(int c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '/' || c == '%';
}
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void skip_ws(Lex *L)
{
    while (L->p < L->end) {
        if (is_ws(*L->p)) {
            L->p++;
        } else if (*L->p == '%') {
            while (L->p < L->end && *L->p != '\n' && *L->p != '\r')
                L->p++;
        } else {
            break;
        }
    }
}

/* Parses an integer at p (no sign); -1 if there is none. */
static long lex_uint(Lex *L)
{
    if (L->p >= L->end || !is_digit(*L->p))
        return -1;
    long v = 0;
    while (L->p < L->end && is_digit(*L->p)) {
        if (v < 100000000L)
            v = v * 10 + (*L->p - '0');
        L->p++;
    }
    return v;
}

typedef struct {
    Obj *v;
    int n, cap;
} Vec;

static int vec_push(Vec *v, const Obj *o)
{
    if (v->n == v->cap) {
        int nc = v->cap ? v->cap * 2 : 16;
        Obj *nv = realloc(v->v, sizeof(Obj) * nc);
        if (!nv)
            return -1;
        v->v = nv;
        v->cap = nc;
    }
    v->v[v->n++] = *o;
    return 0;
}

static int parse_value(Lex *L, Obj *o, int depth);

static int finish_vec(Lex *L, Vec *v, Obj *o, int type)
{
    o->t = type;
    o->n = type == O_DICT ? v->n / 2 : v->n;
    o->u.a = NULL;
    if (v->n) {
        o->u.a = aalloc(L->d, sizeof(Obj) * v->n);
        if (!o->u.a) {
            free(v->v);
            return -1;
        }
        memcpy(o->u.a, v->v, sizeof(Obj) * v->n);
    }
    free(v->v);
    return 0;
}

static int parse_value(Lex *L, Obj *o, int depth)
{
    memset(o, 0, sizeof(*o));
    skip_ws(L);
    if (L->p >= L->end) {
        L->trunc = 1;
        return -1;
    }
    if (depth > 32) {
        L->p++;
        return -1;
    }
    int c = *L->p;
    if (c == '/') { /* name */
        const uint8_t *s = ++L->p;
        while (L->p < L->end && !is_ws(*L->p) && !is_delim(*L->p))
            L->p++;
        int n = (int)(L->p - s);
        char *out = aalloc(L->d, n + 1);
        if (!out)
            return -1;
        int k = 0;
        for (int i = 0; i < n; i++) {
            if (s[i] == '#' && i + 2 < n && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
                out[k++] = (char)(hexval(s[i + 1]) << 4 | hexval(s[i + 2]));
                i += 2;
            } else {
                out[k++] = (char)s[i];
            }
        }
        out[k] = 0;
        o->t = O_NAME;
        o->u.s = out;
        o->n = k;
        if (L->p >= L->end)
            L->trunc = 1; /* a name may continue past the buffer */
        return 0;
    }
    if (c == '(') { /* literal string: find the end first, then decode */
        const uint8_t *s = ++L->p, *q = s;
        int nest = 1;
        while (q < L->end) {
            if (*q == '\\') { q += 2; continue; }
            if (*q == '(') nest++;
            else if (*q == ')' && --nest == 0) break;
            q++;
        }
        if (q >= L->end) {
            L->trunc = 1;
            L->p = L->end;
            return -1;
        }
        char *out = aalloc(L->d, (size_t)(q - s) + 1);
        if (!out)
            return -1;
        int k = 0;
        for (const uint8_t *p = s; p < q; p++) {
            if (*p != '\\') {
                if (*p == '\r') { /* an end of line in a string is a \n */
                    out[k++] = '\n';
                    if (p + 1 < q && p[1] == '\n') p++;
                    continue;
                }
                out[k++] = (char)*p;
                continue;
            }
            if (++p >= q)
                break;
            switch (*p) {
            case 'n': out[k++] = '\n'; break;
            case 'r': out[k++] = '\r'; break;
            case 't': out[k++] = '\t'; break;
            case 'b': out[k++] = '\b'; break;
            case 'f': out[k++] = '\f'; break;
            case '\r': if (p + 1 < q && p[1] == '\n') p++; break; /* line continuation */
            case '\n': break;
            default:
                if (*p >= '0' && *p <= '7') {
                    int v = 0;
                    for (int i = 0; i < 3 && p < q && *p >= '0' && *p <= '7'; i++, p++)
                        v = v * 8 + (*p - '0');
                    p--;
                    out[k++] = (char)v;
                } else {
                    out[k++] = (char)*p;
                }
            }
        }
        out[k] = 0;
        L->p = q + 1;
        o->t = O_STR;
        o->u.s = out;
        o->n = k;
        return 0;
    }
    if (c == '<' && L->p + 1 < L->end && L->p[1] == '<') { /* dictionary */
        L->p += 2;
        Vec v = {0};
        for (;;) {
            skip_ws(L);
            if (L->p >= L->end) {
                L->trunc = 1;
                free(v.v);
                return -1;
            }
            if (*L->p == '>' && L->p + 1 < L->end && L->p[1] == '>') {
                L->p += 2;
                break;
            }
            Obj k, val;
            if (parse_value(L, &k, depth + 1) < 0) {
                if (L->trunc || L->p >= L->end) { free(v.v); return -1; }
                continue;
            }
            if (k.t != O_NAME)
                continue; /* junk where a key should be */
            skip_ws(L);
            if (L->p < L->end && *L->p == '>' && L->p + 1 < L->end && L->p[1] == '>') {
                val.t = O_NULL; /* a key without a value */
            } else if (parse_value(L, &val, depth + 1) < 0) {
                if (L->trunc) { free(v.v); return -1; }
                val.t = O_NULL;
            }
            if (vec_push(&v, &k) < 0 || vec_push(&v, &val) < 0) {
                free(v.v);
                return -1;
            }
        }
        return finish_vec(L, &v, o, O_DICT);
    }
    if (c == '<') { /* hex string */
        const uint8_t *s = ++L->p, *q = s;
        while (q < L->end && *q != '>')
            q++;
        if (q >= L->end) {
            L->trunc = 1;
            L->p = L->end;
            return -1;
        }
        char *out = aalloc(L->d, (size_t)(q - s) / 2 + 2);
        if (!out)
            return -1;
        int k = 0, hi = -1;
        for (const uint8_t *p = s; p < q; p++) {
            int h = hexval(*p);
            if (h < 0)
                continue;
            if (hi < 0) {
                hi = h;
            } else {
                out[k++] = (char)(hi << 4 | h);
                hi = -1;
            }
        }
        if (hi >= 0)
            out[k++] = (char)(hi << 4);
        out[k] = 0;
        L->p = q + 1;
        o->t = O_STR;
        o->u.s = out;
        o->n = k;
        return 0;
    }
    if (c == '[') {
        L->p++;
        Vec v = {0};
        for (;;) {
            skip_ws(L);
            if (L->p >= L->end) {
                L->trunc = 1;
                free(v.v);
                return -1;
            }
            if (*L->p == ']') {
                L->p++;
                break;
            }
            Obj it;
            if (parse_value(L, &it, depth + 1) < 0) {
                if (L->trunc || L->p >= L->end) { free(v.v); return -1; }
                continue;
            }
            if (vec_push(&v, &it) < 0) {
                free(v.v);
                return -1;
            }
        }
        return finish_vec(L, &v, o, O_ARR);
    }
    if (is_digit(c) || c == '-' || c == '+' || c == '.') {
        int neg = 0, frac = 0, any = 0;
        double v = 0, scale = 1;
        if (c == '-' || c == '+') {
            neg = c == '-';
            L->p++;
            while (L->p < L->end && (*L->p == '-' || *L->p == '+')) /* "--5" happens */
                L->p++;
        }
        while (L->p < L->end && (is_digit(*L->p) || *L->p == '.')) {
            if (*L->p == '.') {
                if (frac) break;
                frac = 1;
            } else {
                any = 1;
                if (frac) {
                    scale /= 10;
                    v += (*L->p - '0') * scale;
                } else {
                    v = v * 10 + (*L->p - '0');
                }
            }
            L->p++;
        }
        o->t = O_NUM;
        o->num = (float)(neg ? -v : v);
        o->isint = !frac;
        if (!any)
            o->num = 0;
        if (L->p >= L->end)
            L->trunc = 1;
        /* "12 0 R" */
        if (!frac && !neg && any) {
            const uint8_t *save = L->p;
            int save_trunc = L->trunc;
            skip_ws(L);
            long gen = lex_uint(L);
            if (gen >= 0) {
                skip_ws(L);
                if (L->p < L->end && *L->p == 'R' && (L->p + 1 >= L->end || is_ws(L->p[1]) || is_delim(L->p[1]))) {
                    L->p++;
                    o->t = O_REF;
                    o->n = (int)v;
                    return 0;
                }
                if (L->p >= L->end)
                    L->trunc = 1;
            }
            if (!L->trunc || save_trunc)
                L->trunc = save_trunc;
            L->p = save;
        }
        return 0;
    }
    if (is_delim(c)) { /* a stray ) > ] } */
        L->p++;
        return -1;
    }
    const uint8_t *s = L->p;
    while (L->p < L->end && !is_ws(*L->p) && !is_delim(*L->p))
        L->p++;
    int n = (int)(L->p - s);
    if (n == 4 && !memcmp(s, "true", 4)) { o->t = O_BOOL; o->n = 1; return 0; }
    if (n == 5 && !memcmp(s, "false", 5)) { o->t = O_BOOL; return 0; }
    if (n == 4 && !memcmp(s, "null", 4)) { o->t = O_NULL; return 0; }
    char *out = aalloc(L->d, n + 1);
    if (!out)
        return -1;
    memcpy(out, s, n);
    out[n] = 0;
    o->t = O_KW;
    o->u.s = out;
    o->n = n;
    return 0;
}

static int is_kw(const Obj *o, const char *k) { return o && o->t == O_KW && !strcmp(o->u.s, k); }
static int is_name(const Obj *o, const char *k) { return o && o->t == O_NAME && !strcmp(o->u.s, k); }

static Obj *dget(Obj *dict, const char *key)
{
    if (!dict || dict->t != O_DICT)
        return NULL;
    for (int i = 0; i < dict->n; i++)
        if (!strcmp(dict->u.a[2 * i].u.s, key))
            return &dict->u.a[2 * i + 1];
    return NULL;
}

static Obj *load_obj(PdfDoc *d, int num);

static Obj *res(PdfDoc *d, Obj *o)
{
    for (int guard = 0; o && o->t == O_REF && guard < 8; guard++)
        o = load_obj(d, o->n);
    return o && o->t == O_REF ? NULL : o;
}

static Obj *dgetr(PdfDoc *d, Obj *dict, const char *key) { return res(d, dget(dict, key)); }

static int onum(Obj *o, float *v)
{
    if (!o || o->t != O_NUM)
        return 0;
    *v = o->num;
    return 1;
}

static int dint(PdfDoc *d, Obj *dict, const char *key, int def)
{
    float v;
    return onum(dgetr(d, dict, key), &v) ? (int)v : def;
}

/* --------------------------------------------------------------- objects */

/* Parses the value at file offset off (after "N G obj" if header). */
static Obj *parse_at(PdfDoc *d, uint32_t off, int header, int expect)
{
    for (int win = 4096;; win *= 4) {
        if (win > (1 << 20))
            win = 1 << 20;
        uint8_t *buf = malloc(win);
        if (!buf)
            return NULL;
        int n = rd(d, off, buf, win);
        size_t mark = d->ar_used;
        Lex L = {buf, buf + n, d, 0};
        Obj *v = NULL;
        int ok = 1;
        if (header) {
            skip_ws(&L);
            long num = lex_uint(&L);
            skip_ws(&L);
            long gen = lex_uint(&L);
            skip_ws(&L);
            Obj k;
            if (num < 0 || gen < 0 || parse_value(&L, &k, 0) < 0 || !is_kw(&k, "obj") || (expect && num != expect))
                ok = 0;
        }
        if (ok) {
            v = aalloc(d, sizeof(Obj));
            if (!v || parse_value(&L, v, 0) < 0)
                ok = 0;
        }
        if (ok && v->t == O_DICT) {
            skip_ws(&L);
            if (L.end - L.p >= 7 && !memcmp(L.p, "stream", 6)) {
                L.p += 6;
                if (L.p < L.end && *L.p == '\r') L.p++;
                if (L.p < L.end && *L.p == '\n') L.p++;
                v->stream = 1;
                v->soff = off + (uint32_t)(L.p - buf);
            } else if (L.end - L.p < 8 && n == win) {
                L.trunc = 1;
                ok = 0;
            }
        }
        int retry = !ok && L.trunc && n == win && win < (1 << 20);
        free(buf);
        if (ok)
            return v;
        d->ar_used = mark;
        if (!retry)
            return NULL;
    }
}

static uint8_t *stream_bytes(PdfDoc *d, Obj *s, int *outlen);

static ObjStm *get_stm(PdfDoc *d, int num)
{
    ObjStm *lru = &d->stm[0];
    for (int i = 0; i < OBJSTM_CACHE; i++) {
        if (d->stm[i].data && d->stm[i].num == num) {
            d->stm[i].stamp = ++d->stamp;
            return &d->stm[i];
        }
        if (d->stm[i].stamp < lru->stamp)
            lru = &d->stm[i];
    }
    Obj *so = load_obj(d, num);
    if (!so || !so->stream)
        return NULL;
    int n = dint(d, so, "N", 0), first = dint(d, so, "First", 0);
    if (n <= 0 || n > 100000 || first < 0)
        return NULL;
    int len;
    uint8_t *data = stream_bytes(d, so, &len);
    if (!data)
        return NULL;
    int *nums = malloc(sizeof(int) * n * 2);
    if (!nums || first > len) {
        free(nums);
        free(data);
        return NULL;
    }
    Lex L = {data, data + first, d, 0};
    int k = 0;
    for (; k < n; k++) {
        skip_ws(&L);
        long a = lex_uint(&L);
        skip_ws(&L);
        long b = lex_uint(&L);
        if (a < 0 || b < 0)
            break;
        nums[k] = (int)a;
        nums[n + k] = (int)b;
    }
    free(lru->data);
    free(lru->nums);
    lru->num = num;
    lru->data = data;
    lru->len = len;
    lru->first = first;
    lru->n = k;
    lru->nums = nums;
    lru->offs = nums + n;
    lru->stamp = ++d->stamp;
    return lru;
}

static Obj *load_from_stm(PdfDoc *d, int stm, int idx, int num)
{
    ObjStm *s = get_stm(d, stm);
    if (!s)
        return NULL;
    int i = idx < s->n && s->nums[idx] == num ? idx : -1;
    for (int k = 0; i < 0 && k < s->n; k++)
        if (s->nums[k] == num)
            i = k;
    if (i < 0)
        return NULL;
    int off = s->first + s->offs[i];
    if (off < 0 || off >= s->len)
        return NULL;
    Lex L = {s->data + off, s->data + s->len, d, 0};
    Obj *v = aalloc(d, sizeof(Obj));
    if (!v || parse_value(&L, v, 0) < 0)
        return NULL;
    return v;
}

static Obj *load_obj(PdfDoc *d, int num)
{
    if (num <= 0 || num >= d->nx || d->depth > 12)
        return NULL;
    int h = num & 511;
    if (d->cache[h].num == num)
        return d->cache[h].o;
    XEnt *e = &d->x[num];
    Obj *o = NULL;
    d->depth++;
    if (e->type == X_OFF)
        o = parse_at(d, e->off, 1, num);
    else if (e->type == X_STM)
        o = load_from_stm(d, (int)e->off, e->idx, num);
    d->depth--;
    if (o) {
        d->cache[h].num = num;
        d->cache[h].o = o;
    }
    return o;
}

/* ---------------------------------------------------------------- filters */

static uint8_t *do_inflate(const uint8_t *in, int n, int *outlen, int raw)
{
    z_stream z;
    memset(&z, 0, sizeof(z));
    if ((raw ? inflateInit2(&z, -15) : inflateInit(&z)) != Z_OK)
        return NULL;
    int cap = n * 4 + 1024;
    if (cap > MAX_STREAM) cap = MAX_STREAM;
    uint8_t *out = malloc(cap);
    if (!out) {
        inflateEnd(&z);
        return NULL;
    }
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)n;
    int len = 0, r;
    for (;;) {
        z.next_out = out + len;
        z.avail_out = (uInt)(cap - len);
        r = inflate(&z, Z_NO_FLUSH);
        len = cap - (int)z.avail_out;
        if (r == Z_STREAM_END)
            break;
        if (z.avail_out == 0) {
            if (cap >= MAX_STREAM)
                break;
            int nc = cap * 2 > MAX_STREAM ? MAX_STREAM : cap * 2;
            uint8_t *no = realloc(out, nc);
            if (!no)
                break;
            out = no;
            cap = nc;
            continue;
        }
        if (r != Z_OK || z.avail_in == 0)
            break; /* damaged or cut short: keep what came out */
    }
    inflateEnd(&z);
    if (len == 0 && !raw && r == Z_DATA_ERROR) {
        free(out);
        return do_inflate(in, n, outlen, 1); /* no zlib header */
    }
    *outlen = len;
    return out;
}

static uint8_t *png_unpredict(uint8_t *in, int n, int colors, int bpc, int cols, int *outlen)
{
    int rowlen = (colors * bpc * cols + 7) / 8, bpp = (colors * bpc + 7) / 8;
    if (rowlen <= 0 || bpp <= 0)
        return NULL;
    int rows = n / (rowlen + 1);
    uint8_t *out = malloc((size_t)rows * rowlen + 1);
    if (!out)
        return NULL;
    for (int r = 0; r < rows; r++) {
        const uint8_t *src = in + (size_t)r * (rowlen + 1);
        uint8_t *cur = out + (size_t)r * rowlen, *prev = r ? cur - rowlen : NULL;
        int type = src[0];
        src++;
        for (int x = 0; x < rowlen; x++) {
            int a = x >= bpp ? cur[x - bpp] : 0, b = prev ? prev[x] : 0, c = prev && x >= bpp ? prev[x - bpp] : 0;
            int v = src[x];
            switch (type) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: {
                int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                break;
            }
            }
            cur[x] = (uint8_t)v;
        }
    }
    *outlen = rows * rowlen;
    return out;
}

static uint8_t *ahx_decode(const uint8_t *in, int n, int *outlen)
{
    uint8_t *out = malloc(n / 2 + 2);
    if (!out)
        return NULL;
    int k = 0, hi = -1;
    for (int i = 0; i < n && in[i] != '>'; i++) {
        int h = hexval(in[i]);
        if (h < 0)
            continue;
        if (hi < 0) hi = h;
        else { out[k++] = (uint8_t)(hi << 4 | h); hi = -1; }
    }
    if (hi >= 0)
        out[k++] = (uint8_t)(hi << 4);
    *outlen = k;
    return out;
}

static uint8_t *a85_decode(const uint8_t *in, int n, int *outlen)
{
    uint8_t *out = malloc((size_t)n * 4 / 5 + 8);
    if (!out)
        return NULL;
    int k = 0, cnt = 0;
    uint32_t v = 0;
    int i = 0;
    if (n >= 2 && in[0] == '<' && in[1] == '~')
        i = 2;
    for (; i < n; i++) {
        int c = in[i];
        if (c == '~')
            break;
        if (is_ws(c))
            continue;
        if (c == 'z' && cnt == 0) {
            memset(out + k, 0, 4);
            k += 4;
            continue;
        }
        if (c < '!' || c > 'u')
            continue;
        v = v * 85 + (uint32_t)(c - '!');
        if (++cnt == 5) {
            out[k++] = (uint8_t)(v >> 24); out[k++] = (uint8_t)(v >> 16);
            out[k++] = (uint8_t)(v >> 8); out[k++] = (uint8_t)v;
            cnt = 0;
            v = 0;
        }
    }
    if (cnt > 1) {
        for (int j = cnt; j < 5; j++)
            v = v * 85 + 84;
        for (int j = 0; j < cnt - 1; j++)
            out[k++] = (uint8_t)(v >> (24 - 8 * j));
    }
    *outlen = k;
    return out;
}

static uint8_t *rl_decode(const uint8_t *in, int n, int *outlen)
{
    int cap = n * 4 + 256, k = 0;
    uint8_t *out = malloc(cap);
    for (int i = 0; out && i < n;) {
        int l = in[i++];
        if (l == 128)
            break;
        int cnt = l < 128 ? l + 1 : 257 - l;
        if (k + cnt > cap) {
            if (cap >= MAX_STREAM) break;
            cap = cap * 2 + cnt;
            uint8_t *no = realloc(out, cap);
            if (!no) break;
            out = no;
        }
        if (l < 128) {
            if (i + cnt > n) cnt = n - i;
            memcpy(out + k, in + i, cnt);
            i += cnt;
        } else {
            if (i >= n) break;
            memset(out + k, in[i++], cnt);
        }
        k += cnt;
    }
    *outlen = k;
    return out;
}

static uint8_t *lzw_decode(const uint8_t *in, int n, int early, int *outlen)
{
    enum { MAXC = 4096 };
    static uint16_t prefix[MAXC];
    static uint8_t suffix[MAXC], first[MAXC];
    static uint16_t len_of[MAXC];
    int cap = n * 3 + 1024, k = 0;
    uint8_t *out = malloc(cap);
    if (!out)
        return NULL;
    for (int i = 0; i < 256; i++) {
        suffix[i] = first[i] = (uint8_t)i;
        len_of[i] = 1;
        prefix[i] = 0xFFFF;
    }
    int next = 258, bits = 9, prev = -1;
    uint32_t buf = 0;
    int nb = 0, pos = 0;
    for (;;) {
        while (nb < bits && pos < n) {
            buf = buf << 8 | in[pos++];
            nb += 8;
        }
        if (nb < bits)
            break;
        int code = (int)((buf >> (nb - bits)) & ((1u << bits) - 1));
        nb -= bits;
        if (code == 256) {
            next = 258;
            bits = 9;
            prev = -1;
            continue;
        }
        if (code == 257)
            break;
        int cur;
        if (code < next) cur = code;
        else if (code == next && prev >= 0) cur = prev;
        else break;
        int l = len_of[cur] + (code == next ? 1 : 0);
        if (k + l > cap) {
            if (cap >= MAX_STREAM) break;
            cap *= 2;
            uint8_t *no = realloc(out, cap);
            if (!no) break;
            out = no;
        }
        int c = cur, p = k + len_of[cur] - 1;
        while (c != 0xFFFF && p >= k) {
            out[p--] = suffix[c];
            c = prefix[c];
        }
        if (code == next)
            out[k + l - 1] = first[prev];
        if (prev >= 0 && next < MAXC) {
            prefix[next] = (uint16_t)prev;
            suffix[next] = first[code == next ? prev : cur];
            first[next] = first[prev];
            len_of[next] = (uint16_t)(len_of[prev] + 1);
            next++;
        }
        k += l;
        prev = code;
        if (next + early >= (1 << bits) && bits < 12)
            bits++;
    }
    *outlen = k;
    return out;
}

/* Scans from off for "endstream"; returns the data length before it. */
static int find_endstream(PdfDoc *d, uint32_t off)
{
    enum { CH = 65536 };
    uint8_t *buf = malloc(CH + 16);
    if (!buf)
        return -1;
    int found = -1;
    for (uint32_t pos = off; pos < d->size && found < 0 && pos - off < (uint32_t)MAX_STREAM; pos += CH) {
        int n = rd(d, pos, buf, CH + 9);
        for (int i = 0; i + 9 <= n; i++)
            if (buf[i] == 'e' && !memcmp(buf + i, "endstream", 9)) {
                found = (int)(pos - off) + i;
                break;
            }
        if (n < CH)
            break;
    }
    free(buf);
    while (found > 0) { /* the EOL before endstream isn't data */
        uint8_t c;
        if (rd(d, off + found - 1, &c, 1) != 1 || (c != '\n' && c != '\r'))
            break;
        found--;
    }
    return found;
}

static int stream_len(PdfDoc *d, Obj *s)
{
    float lf;
    int len = -1;
    if (onum(dgetr(d, s, "Length"), &lf) && lf >= 0)
        len = (int)lf;
    if (len < 0 || (uint64_t)s->soff + (uint64_t)len > d->size) {
        len = find_endstream(d, s->soff);
    } else { /* check that endstream follows; generators get /Length wrong */
        uint8_t tail[32];
        int n = rd(d, s->soff + len, tail, sizeof(tail));
        int ok = 0;
        for (int i = 0; i + 9 <= n; i++) {
            if (!memcmp(tail + i, "endstream", 9)) { ok = 1; break; }
            if (!is_ws(tail[i])) break;
        }
        if (!ok)
            len = find_endstream(d, s->soff);
    }
    return len;
}

/* Decoded stream data (malloc'd), or NULL. Image filters stop decoding. */
static uint8_t *stream_bytes(PdfDoc *d, Obj *s, int *outlen)
{
    if (!s || !s->stream)
        return NULL;
    int len = stream_len(d, s);
    if (len < 0 || len > MAX_STREAM)
        return NULL;
    uint8_t *data = malloc(len + 1);
    if (!data)
        return NULL;
    len = rd(d, s->soff, data, len);
    Obj *f = dgetr(d, s, "Filter"), *p = dgetr(d, s, "DecodeParms");
    if (!f) f = dgetr(d, s, "F");
    if (!p) p = dgetr(d, s, "DP");
    int nf = !f ? 0 : f->t == O_ARR ? f->n : 1;
    for (int i = 0; i < nf && data; i++) {
        Obj *fi = f->t == O_ARR ? res(d, &f->u.a[i]) : f;
        Obj *pi = !p ? NULL : p->t == O_ARR ? (i < p->n ? res(d, &p->u.a[i]) : NULL) : p;
        if (!fi || fi->t != O_NAME) {
            free(data);
            return NULL;
        }
        const char *nm = fi->u.s;
        uint8_t *nd = NULL;
        int nl = 0;
        if (!strcmp(nm, "FlateDecode") || !strcmp(nm, "Fl") || !strcmp(nm, "LZWDecode") || !strcmp(nm, "LZW")) {
            if (nm[0] == 'F')
                nd = do_inflate(data, len, &nl, 0);
            else
                nd = lzw_decode(data, len, dint(d, pi, "EarlyChange", 1), &nl);
            int pred = dint(d, pi, "Predictor", 1);
            if (nd && pred >= 10) {
                uint8_t *u = png_unpredict(nd, nl, dint(d, pi, "Colors", 1), dint(d, pi, "BitsPerComponent", 8),
                                           dint(d, pi, "Columns", 1), &nl);
                free(nd);
                nd = u;
            }
        } else if (!strcmp(nm, "ASCIIHexDecode") || !strcmp(nm, "AHx")) {
            nd = ahx_decode(data, len, &nl);
        } else if (!strcmp(nm, "ASCII85Decode") || !strcmp(nm, "A85")) {
            nd = a85_decode(data, len, &nl);
        } else if (!strcmp(nm, "RunLengthDecode") || !strcmp(nm, "RL")) {
            nd = rl_decode(data, len, &nl);
        }
        free(data);
        data = nd;
        len = nl;
    }
    if (!data)
        return NULL;
    *outlen = len;
    return data;
}

/* ------------------------------------------------------------ cross refs */

static int grow_x(PdfDoc *d, int num)
{
    if (num < 0 || num >= MAX_OBJS)
        return -1;
    if (num >= d->nx) {
        int nn = d->nx ? d->nx : 64;
        while (nn <= num)
            nn *= 2;
        if (nn > MAX_OBJS) nn = MAX_OBJS;
        XEnt *nx = realloc(d->x, sizeof(XEnt) * nn);
        if (!nx)
            return -1;
        memset(nx + d->nx, 0, sizeof(XEnt) * (nn - d->nx));
        d->x = nx;
        d->nx = nn;
    }
    return 0;
}

/* Newest section first, so an entry already set is kept. A free entry gives
 * way to an object stream entry: hybrid files list their compressed objects
 * as free in the table and find them in the XRefStm. */
static void set_x(PdfDoc *d, int num, int type, uint32_t a, int idx)
{
    if (num <= 0 || grow_x(d, num) < 0)
        return;
    int cur = d->x[num].type;
    if (cur != X_UNSET && !(cur == X_FREE && type == X_STM))
        return;
    d->x[num].type = (uint8_t)type;
    d->x[num].off = a;
    d->x[num].idx = (uint16_t)idx;
}

static void read_trailer(PdfDoc *d, Obj *t, long *prev, long *xstm)
{
    Obj *r = dget(t, "Root");
    if (r && r->t == O_REF && !d->root)
        d->root = r->n;
    if (dget(t, "Encrypt"))
        d->encrypted = 1;
    float v;
    if (onum(dget(t, "Prev"), &v)) *prev = (long)v;
    if (onum(dget(t, "XRefStm"), &v)) *xstm = (long)v;
}

static int xref_stream(PdfDoc *d, Obj *o, long *prev)
{
    if (!o->stream || !is_name(dget(o, "Type"), "XRef"))
        return -1;
    Obj *w = dget(o, "W");
    if (!w || w->t != O_ARR || w->n < 3)
        return -1;
    int W[3], rowlen = 0;
    for (int i = 0; i < 3; i++) {
        float f = 0;
        onum(&w->u.a[i], &f);
        W[i] = (int)f;
        if (W[i] < 0 || W[i] > 8)
            return -1;
        rowlen += W[i];
    }
    long xs = -1;
    read_trailer(d, o, prev, &xs);
    int size = dint(d, o, "Size", 0), len;
    uint8_t *data = stream_bytes(d, o, &len);
    if (!data || rowlen == 0) {
        free(data);
        return -1;
    }
    Obj *ix = dget(o, "Index");
    int pairs = ix && ix->t == O_ARR ? ix->n / 2 : 1;
    const uint8_t *p = data, *end = data + len;
    for (int k = 0; k < pairs; k++) {
        float a = 0, b = (float)size;
        if (ix && ix->t == O_ARR) {
            onum(&ix->u.a[2 * k], &a);
            onum(&ix->u.a[2 * k + 1], &b);
        }
        for (int i = 0; i < (int)b && p + rowlen <= end; i++, p += rowlen) {
            uint32_t f[3] = {W[0] ? 0 : 1, 0, 0};
            const uint8_t *q = p;
            for (int j = 0; j < 3; j++) {
                if (!W[j]) continue;
                uint32_t v = 0;
                for (int b2 = 0; b2 < W[j]; b2++)
                    v = v << 8 | *q++;
                f[j] = v;
            }
            int num = (int)a + i;
            if (f[0] == 1) set_x(d, num, X_OFF, f[1], 0);
            else if (f[0] == 2) set_x(d, num, X_STM, f[1], (int)f[2]);
            else if (f[0] == 0) set_x(d, num, X_FREE, 0, 0);
        }
    }
    free(data);
    return 0;
}

static int xref_table(PdfDoc *d, uint32_t off, long *prev, long *xstm)
{
    uint32_t pos = off + 4;
    enum { CHUNK = 4096 };
    uint8_t *buf = malloc(CHUNK * 20 + 64);
    if (!buf)
        return -1;
    for (int guard = 0; guard < 100000; guard++) {
        int n = rd(d, pos, buf, 64);
        Lex L = {buf, buf + n, d, 0};
        skip_ws(&L);
        if (L.end - L.p >= 7 && !memcmp(L.p, "trailer", 7)) {
            size_t mark = d->ar_used;
            Obj *t = parse_at(d, pos + (uint32_t)(L.p - buf) + 7, 0, 0);
            if (t)
                read_trailer(d, t, prev, xstm);
            d->ar_used = mark;
            free(buf);
            return t ? 0 : -1;
        }
        long start = lex_uint(&L);
        skip_ws(&L);
        long count = lex_uint(&L);
        if (start < 0 || count < 0) {
            free(buf);
            return -1;
        }
        skip_ws(&L);
        pos += (uint32_t)(L.p - buf);
        for (long done = 0; done < count;) {
            int want = count - done > CHUNK ? CHUNK : (int)(count - done);
            int got = rd(d, pos, buf, want * 20 + 32);
            Lex E = {buf, buf + got, d, 0};
            int k;
            for (k = 0; k < want; k++) {
                skip_ws(&E);
                const uint8_t *row = E.p;
                long o = lex_uint(&E);
                skip_ws(&E);
                long g = lex_uint(&E);
                skip_ws(&E);
                if (o < 0 || g < 0 || E.p >= E.end || (*E.p != 'n' && *E.p != 'f')) {
                    E.p = row;
                    break;
                }
                int num = (int)(start + done + k);
                if (*E.p == 'n') set_x(d, num, X_OFF, (uint32_t)o, 0);
                else set_x(d, num, X_FREE, 0, 0);
                E.p++;
            }
            done += k;
            pos += (uint32_t)(E.p - buf);
            if (k == 0) {
                free(buf);
                return -1;
            }
        }
    }
    free(buf);
    return -1;
}

static int read_xref(PdfDoc *d)
{
    uint8_t tail[1024 + 1];
    uint32_t toff = d->size > 1024 ? d->size - 1024 : 0;
    int n = rd(d, toff, tail, 1024);
    tail[n] = 0;
    long sx = -1;
    for (int i = n - 9; i >= 0; i--)
        if (!memcmp(tail + i, "startxref", 9)) {
            Lex L = {tail + i + 9, tail + n, d, 0};
            skip_ws(&L);
            sx = lex_uint(&L);
            break;
        }
    if (sx < 0 || (uint32_t)sx >= d->size)
        return -1;
    long off = sx;
    for (int guard = 0; guard < 64 && off > 0 && off < (long)d->size; guard++) {
        long prev = -1, xstm = -1;
        uint8_t head[8];
        rd(d, (uint32_t)off, head, 8);
        int r;
        if (!memcmp(head, "xref", 4)) {
            r = xref_table(d, (uint32_t)off, &prev, &xstm);
            if (r == 0 && xstm > 0) { /* hybrid file: the stream's entries come after the table's */
                long p2 = -1;
                size_t mark = d->ar_used;
                Obj *o = parse_at(d, (uint32_t)xstm, 1, 0);
                if (o) xref_stream(d, o, &p2);
                d->ar_used = mark;
            }
        } else {
            size_t mark = d->ar_used;
            Obj *o = parse_at(d, (uint32_t)off, 1, 0);
            r = o ? xref_stream(d, o, &prev) : -1;
            d->ar_used = mark;
        }
        if (r < 0)
            return guard ? 0 : -1;
        if (prev == off)
            break;
        off = prev;
    }
    return 0;
}

/* Broken cross references: find every "N G obj" and "trailer" in the file. */
static void scan_objects(PdfDoc *d)
{
    enum { CH = 65536, OV = 32 };
    memset(d->x, 0, sizeof(XEnt) * d->nx);
    d->root = 0;
    uint8_t *buf = malloc(CH + OV);
    if (!buf)
        return;
    for (uint32_t pos = 0; pos < d->size; pos += CH) {
        int n = rd(d, pos, buf, CH + OV);
        int lim = n < CH ? n : CH;
        for (int i = 0; i < lim; i++) {
            if (buf[i] == 'o' && i + 3 <= n && !memcmp(buf + i, "obj", 3) &&
                (i + 3 == n || is_ws(buf[i + 3]) || is_delim(buf[i + 3]))) {
                int j = i - 1;
                while (j >= 0 && is_ws(buf[j])) j--;
                int ge = j;
                while (j >= 0 && is_digit(buf[j])) j--;
                if (j == ge) continue;
                int ws = j;
                while (j >= 0 && is_ws(buf[j])) j--;
                if (j == ws) continue;
                int ne = j;
                while (j >= 0 && is_digit(buf[j])) j--;
                if (j == ne || (j >= 0 && !is_ws(buf[j]) && !is_delim(buf[j]))) continue;
                long num = 0;
                for (int k = j + 1; k <= ne; k++) num = num * 10 + (buf[k] - '0');
                if (num > 0 && num < MAX_OBJS && grow_x(d, (int)num) == 0) {
                    d->x[num].type = X_OFF; /* the last one in the file wins */
                    d->x[num].off = pos + (uint32_t)(j + 1);
                }
            } else if (buf[i] == 't' && i + 7 <= n && !memcmp(buf + i, "trailer", 7)) {
                size_t mark = d->ar_used;
                Obj *t = parse_at(d, pos + i + 7, 0, 0);
                Obj *r = dget(t, "Root");
                if (r && r->t == O_REF) d->root = r->n;
                if (dget(t, "Encrypt")) d->encrypted = 1;
                d->ar_used = mark;
            }
        }
        if (n < CH)
            break;
    }
    free(buf);
    /* objects inside object streams, and the catalog if no trailer named it */
    int nx = d->nx;
    for (int num = 1; num < nx; num++) {
        if (d->x[num].type != X_OFF)
            continue;
        areset(d);
        Obj *o = load_obj(d, num);
        if (!o || o->t != O_DICT)
            continue;
        Obj *t = dget(o, "Type");
        if (!d->root && is_name(t, "Catalog"))
            d->root = num;
        if (is_name(t, "XRef") && dget(o, "Encrypt"))
            d->encrypted = 1;
        if (is_name(t, "XRef") && !d->root) {
            Obj *r = dget(o, "Root");
            if (r && r->t == O_REF) d->root = r->n;
        }
        if (is_name(t, "ObjStm")) {
            ObjStm *s = get_stm(d, num);
            for (int k = 0; s && k < s->n; k++) {
                int on = s->nums[k];
                if (on > 0 && on < MAX_OBJS && grow_x(d, on) == 0 && d->x[on].type == X_UNSET) {
                    d->x[on].type = X_STM;
                    d->x[on].off = (uint32_t)num;
                    d->x[on].idx = (uint16_t)k;
                }
            }
        }
    }
    areset(d);
}

/* ---------------------------------------------------------------- pages */

static void walk_pages(PdfDoc *d, int num, int owner, int depth)
{
    if (depth > 40 || d->npages >= MAX_PAGES || num <= 0 || num >= d->nx || d->visited[num])
        return;
    d->visited[num] = 1;
    if (d->ar_used > ARENA_SIZE / 2)
        areset(d);
    Obj *node = load_obj(d, num);
    if (!node || node->t != O_DICT)
        return;
    if (dget(node, "Resources"))
        owner = num;
    Obj *kids = dgetr(d, node, "Kids");
    if (kids && kids->t == O_ARR && !is_name(dget(node, "Type"), "Page")) {
        int n = kids->n;
        int *k = malloc(sizeof(int) * (n ? n : 1));
        if (!k)
            return;
        int m = 0;
        for (int i = 0; i < n; i++)
            if (kids->u.a[i].t == O_REF)
                k[m++] = kids->u.a[i].n;
        for (int i = 0; i < m; i++)
            walk_pages(d, k[i], owner, depth + 1);
        free(k);
        return;
    }
    d->pages[d->npages] = num;
    d->res_owner[d->npages] = owner;
    d->npages++;
}

static void find_pages(PdfDoc *d)
{
    d->npages = 0;
    free(d->visited);
    d->visited = calloc(d->nx ? d->nx : 1, 1);
    if (!d->visited)
        return;
    areset(d);
    Obj *cat = load_obj(d, d->root);
    Obj *pages = dget(cat, "Pages");
    if (pages && pages->t == O_REF)
        walk_pages(d, pages->n, 0, 0);
    areset(d);
}

PdfDoc *pdf_open(const char *path, int *err)
{
    int e = PDF_E_MEMORY;
    PdfDoc *d = calloc(1, sizeof(PdfDoc));
    if (!d)
        goto fail;
    d->fd = -1;
    d->ar = malloc(ARENA_SIZE);
    d->pages = malloc(sizeof(int) * MAX_PAGES);
    d->res_owner = malloc(sizeof(int) * MAX_PAGES);
    if (!d->ar || !d->pages || !d->res_owner)
        goto fail;
    d->fd = xio_open(path);
    if (d->fd < 0) {
        e = PDF_E_READ;
        goto fail;
    }
    int64_t sz = xio_size(d->fd);
    if (sz <= 0 || sz > 0x7FFFFFFF) {
        e = PDF_E_READ;
        goto fail;
    }
    d->size = (uint32_t)sz;
    char head[1025];
    int n = rd(d, 0, head, 1024);
    head[n] = 0;
    int ok = 0;
    for (int i = 0; i + 5 <= n; i++)
        if (!memcmp(head + i, "%PDF-", 5)) { ok = 1; break; }
    e = PDF_E_FORMAT;
    if (!ok)
        goto fail;
    if (grow_x(d, 1) < 0)
        goto fail;
    if (read_xref(d) == 0 && d->root)
        find_pages(d);
    if (d->npages == 0) {
        scan_objects(d);
        if (d->root)
            find_pages(d);
    }
    if (d->encrypted) {
        e = PDF_E_ENCRYPTED;
        goto fail;
    }
    if (d->npages == 0)
        goto fail;
    return d;
fail:
    if (err)
        *err = e;
    pdf_close(d);
    return NULL;
}

void pdf_close(PdfDoc *d)
{
    if (!d)
        return;
    if (d->fd >= 0)
        xio_close(d->fd);
    for (int i = 0; i < OBJSTM_CACHE; i++) {
        free(d->stm[i].data);
        free(d->stm[i].nums);
    }
    free(d->x);
    free(d->ar);
    free(d->pages);
    free(d->res_owner);
    free(d->visited);
    free(d);
}

int pdf_page_count(const PdfDoc *d) { return d ? d->npages : 0; }

/* ----------------------------------------------------------------- fonts */

typedef struct { uint16_t lo, hi; uint32_t cp, cp2; } URange;
typedef struct { uint16_t lo, hi; float w; } WRange;

typedef struct {
    int num; /* the font dictionary's object number, -1 if inline */
    char name[64];
    int two;
    URange *u;
    int nu, cu;
    uint32_t enc[256];
    int simple;
    float w[256];
    int has_w;
    WRange *cw;
    int ncw, ccw;
    float dw;
    float wscale; /* Type 3: glyph space to thousandths */
} Font;

static const uint16_t MACROMAN_80[128] = {
    0xC4, 0xC5, 0xC7, 0xC9, 0xD1, 0xD6, 0xDC, 0xE1, 0xE0, 0xE2, 0xE4, 0xE3, 0xE5, 0xE7, 0xE9, 0xE8,
    0xEA, 0xEB, 0xED, 0xEC, 0xEE, 0xEF, 0xF1, 0xF3, 0xF2, 0xF4, 0xF6, 0xF5, 0xFA, 0xF9, 0xFB, 0xFC,
    0x2020, 0xB0, 0xA2, 0xA3, 0xA7, 0x2022, 0xB6, 0xDF, 0xAE, 0xA9, 0x2122, 0xB4, 0xA8, 0x2260, 0xC6, 0xD8,
    0x221E, 0xB1, 0x2264, 0x2265, 0xA5, 0xB5, 0x2202, 0x2211, 0x220F, 0x3C0, 0x222B, 0xAA, 0xBA, 0x3A9, 0xE6, 0xF8,
    0xBF, 0xA1, 0xAC, 0x221A, 0x192, 0x2248, 0x2206, 0xAB, 0xBB, 0x2026, 0xA0, 0xC0, 0xC3, 0xD5, 0x152, 0x153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0xF7, 0x25CA, 0xFF, 0x178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0xB7, 0x201A, 0x201E, 0x2030, 0xC2, 0xCA, 0xC1, 0xCB, 0xC8, 0xCD, 0xCE, 0xCF, 0xCC, 0xD3, 0xD4,
    0xF8FF, 0xD2, 0xDA, 0xDB, 0xD9, 0x131, 0x2C6, 0x2DC, 0xAF, 0x2D8, 0x2D9, 0x2DA, 0xB8, 0x2DD, 0x2DB, 0x2C7,
};

static const struct { const char *n; uint16_t cp; } GLYPHS[] = {
    {"space", 32}, {"exclam", 33}, {"quotedbl", 34}, {"numbersign", 35}, {"dollar", 36}, {"percent", 37},
    {"ampersand", 38}, {"quotesingle", 39}, {"quoteright", 0x2019}, {"parenleft", 40}, {"parenright", 41},
    {"asterisk", 42}, {"plus", 43}, {"comma", 44}, {"hyphen", 45}, {"minus", 0x2212}, {"period", 46},
    {"slash", 47}, {"zero", 48}, {"one", 49}, {"two", 50}, {"three", 51}, {"four", 52}, {"five", 53},
    {"six", 54}, {"seven", 55}, {"eight", 56}, {"nine", 57}, {"colon", 58}, {"semicolon", 59}, {"less", 60},
    {"equal", 61}, {"greater", 62}, {"question", 63}, {"at", 64}, {"bracketleft", 91}, {"backslash", 92},
    {"bracketright", 93}, {"asciicircum", 94}, {"underscore", 95}, {"grave", 96}, {"quoteleft", 0x2018},
    {"braceleft", 123}, {"bar", 124}, {"braceright", 125}, {"asciitilde", 126}, {"quotedblleft", 0x201C},
    {"quotedblright", 0x201D}, {"quotesinglbase", 0x201A}, {"quotedblbase", 0x201E}, {"endash", 0x2013},
    {"emdash", 0x2014}, {"bullet", 0x2022}, {"ellipsis", 0x2026}, {"dagger", 0x2020}, {"daggerdbl", 0x2021},
    {"perthousand", 0x2030}, {"guilsinglleft", 0x2039}, {"guilsinglright", 0x203A}, {"Euro", 0x20AC},
    {"trademark", 0x2122}, {"fi", 0xFB01}, {"fl", 0xFB02}, {"ff", 0xFB00}, {"ffi", 0xFB03}, {"ffl", 0xFB04},
    {"florin", 0x192}, {"circumflex", 0x2C6}, {"tilde", 0x2DC}, {"OE", 0x152}, {"oe", 0x153},
    {"Scaron", 0x160}, {"scaron", 0x161}, {"Zcaron", 0x17D}, {"zcaron", 0x17E}, {"Ydieresis", 0x178},
    {"dotlessi", 0x131}, {"nbspace", 0xA0}, {"sfthyphen", 0xAD},
    /* Latin-1, 0xA1.. */
    {"exclamdown", 0xA1}, {"cent", 0xA2}, {"sterling", 0xA3}, {"currency", 0xA4}, {"yen", 0xA5},
    {"brokenbar", 0xA6}, {"section", 0xA7}, {"dieresis", 0xA8}, {"copyright", 0xA9}, {"ordfeminine", 0xAA},
    {"guillemotleft", 0xAB}, {"logicalnot", 0xAC}, {"registered", 0xAE}, {"macron", 0xAF}, {"degree", 0xB0},
    {"plusminus", 0xB1}, {"twosuperior", 0xB2}, {"threesuperior", 0xB3}, {"acute", 0xB4}, {"mu", 0x3BC},
    {"paragraph", 0xB6}, {"periodcentered", 0xB7}, {"cedilla", 0xB8}, {"onesuperior", 0xB9},
    {"ordmasculine", 0xBA}, {"guillemotright", 0xBB}, {"onequarter", 0xBC}, {"onehalf", 0xBD},
    {"threequarters", 0xBE}, {"questiondown", 0xBF}, {"Agrave", 0xC0}, {"Aacute", 0xC1}, {"Acircumflex", 0xC2},
    {"Atilde", 0xC3}, {"Adieresis", 0xC4}, {"Aring", 0xC5}, {"AE", 0xC6}, {"Ccedilla", 0xC7}, {"Egrave", 0xC8},
    {"Eacute", 0xC9}, {"Ecircumflex", 0xCA}, {"Edieresis", 0xCB}, {"Igrave", 0xCC}, {"Iacute", 0xCD},
    {"Icircumflex", 0xCE}, {"Idieresis", 0xCF}, {"Eth", 0xD0}, {"Ntilde", 0xD1}, {"Ograve", 0xD2},
    {"Oacute", 0xD3}, {"Ocircumflex", 0xD4}, {"Otilde", 0xD5}, {"Odieresis", 0xD6}, {"multiply", 0xD7},
    {"Oslash", 0xD8}, {"Ugrave", 0xD9}, {"Uacute", 0xDA}, {"Ucircumflex", 0xDB}, {"Udieresis", 0xDC},
    {"Yacute", 0xDD}, {"Thorn", 0xDE}, {"germandbls", 0xDF}, {"agrave", 0xE0}, {"aacute", 0xE1},
    {"acircumflex", 0xE2}, {"atilde", 0xE3}, {"adieresis", 0xE4}, {"aring", 0xE5}, {"ae", 0xE6},
    {"ccedilla", 0xE7}, {"egrave", 0xE8}, {"eacute", 0xE9}, {"ecircumflex", 0xEA}, {"edieresis", 0xEB},
    {"igrave", 0xEC}, {"iacute", 0xED}, {"icircumflex", 0xEE}, {"idieresis", 0xEF}, {"eth", 0xF0},
    {"ntilde", 0xF1}, {"ograve", 0xF2}, {"oacute", 0xF3}, {"ocircumflex", 0xF4}, {"otilde", 0xF5},
    {"odieresis", 0xF6}, {"divide", 0xF7}, {"oslash", 0xF8}, {"ugrave", 0xF9}, {"uacute", 0xFA},
    {"ucircumflex", 0xFB}, {"udieresis", 0xFC}, {"yacute", 0xFD}, {"thorn", 0xFE}, {"ydieresis", 0xFF},
    /* Greek */
    {"Alpha", 0x391}, {"Beta", 0x392}, {"Gamma", 0x393}, {"Delta", 0x394}, {"Epsilon", 0x395}, {"Zeta", 0x396},
    {"Eta", 0x397}, {"Theta", 0x398}, {"Iota", 0x399}, {"Kappa", 0x39A}, {"Lambda", 0x39B}, {"Mu", 0x39C},
    {"Nu", 0x39D}, {"Xi", 0x39E}, {"Omicron", 0x39F}, {"Pi", 0x3A0}, {"Rho", 0x3A1}, {"Sigma", 0x3A3},
    {"Tau", 0x3A4}, {"Upsilon", 0x3A5}, {"Phi", 0x3A6}, {"Chi", 0x3A7}, {"Psi", 0x3A8}, {"Omega", 0x3A9},
    {"alpha", 0x3B1}, {"beta", 0x3B2}, {"gamma", 0x3B3}, {"delta", 0x3B4}, {"epsilon", 0x3B5}, {"zeta", 0x3B6},
    {"eta", 0x3B7}, {"theta", 0x3B8}, {"iota", 0x3B9}, {"kappa", 0x3BA}, {"lambda", 0x3BB}, {"nu", 0x3BD},
    {"xi", 0x3BE}, {"omicron", 0x3BF}, {"pi", 0x3C0}, {"rho", 0x3C1}, {"sigma1", 0x3C2}, {"sigma", 0x3C3},
    {"tau", 0x3C4}, {"upsilon", 0x3C5}, {"phi", 0x3C6}, {"chi", 0x3C7}, {"psi", 0x3C8}, {"omega", 0x3C9},
    {"Alphatonos", 0x386}, {"Epsilontonos", 0x388}, {"Etatonos", 0x389}, {"Iotatonos", 0x38A},
    {"Omicrontonos", 0x38C}, {"Upsilontonos", 0x38E}, {"Omegatonos", 0x38F}, {"iotadieresistonos", 0x390},
    {"Iotadieresis", 0x3AA}, {"Upsilondieresis", 0x3AB}, {"alphatonos", 0x3AC}, {"epsilontonos", 0x3AD},
    {"etatonos", 0x3AE}, {"iotatonos", 0x3AF}, {"upsilondieresistonos", 0x3B0}, {"iotadieresis", 0x3CA},
    {"upsilondieresis", 0x3CB}, {"omicrontonos", 0x3CC}, {"upsilontonos", 0x3CD}, {"omegatonos", 0x3CE},
    {"tonos", 0x384}, {"dieresistonos", 0x385}, {"anoteleia", 0xB7},
};

static uint32_t glyph_cp(const char *n)
{
    if (!n[0])
        return 0;
    if (!n[1] && ((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= 'a' && n[0] <= 'z')))
        return (uint8_t)n[0];
    for (int i = 0; i < (int)(sizeof(GLYPHS) / sizeof(GLYPHS[0])); i++)
        if (!strcmp(GLYPHS[i].n, n))
            return GLYPHS[i].cp;
    const char *h = NULL;
    if (!strncmp(n, "uni", 3)) h = n + 3;
    else if (n[0] == 'u' && strlen(n) >= 5 && strlen(n) <= 7) h = n + 1;
    if (h) {
        uint32_t v = 0;
        int k = 0;
        for (; k < 6 && hexval(h[k]) >= 0; k++)
            v = v << 4 | (uint32_t)hexval(h[k]);
        if (k >= 4)
            return !strncmp(n, "uni", 3) ? v >> ((k - 4) * 4) : v;
    }
    const char *dot = strchr(n, '.'); /* "a.sc", "one.oldstyle" */
    if (dot && dot > n && dot - n < 32) {
        char b[32];
        memcpy(b, n, dot - n);
        b[dot - n] = 0;
        return glyph_cp(b);
    }
    return 0;
}

static uint32_t symbol_cp(int c)
{
    static const uint16_t U[26] = {0x391, 0x392, 0x3A7, 0x394, 0x395, 0x3A6, 0x393, 0x397, 0x399, 0x3D1, 0x39A,
                                   0x39B, 0x39C, 0x39D, 0x39F, 0x3A0, 0x398, 0x3A1, 0x3A3, 0x3A4, 0x3A5, 0x3C2,
                                   0x3A9, 0x39E, 0x3A8, 0x396};
    static const uint16_t Lw[26] = {0x3B1, 0x3B2, 0x3C7, 0x3B4, 0x3B5, 0x3C6, 0x3B3, 0x3B7, 0x3B9, 0x3D5, 0x3BA,
                                    0x3BB, 0x3BC, 0x3BD, 0x3BF, 0x3C0, 0x3B8, 0x3C1, 0x3C3, 0x3C4, 0x3C5, 0x3D6,
                                    0x3C9, 0x3BE, 0x3C8, 0x3B6};
    if (c >= 'A' && c <= 'Z') return U[c - 'A'];
    if (c >= 'a' && c <= 'z') return Lw[c - 'a'];
    return c < 128 ? (uint32_t)c : 0;
}

typedef struct {
    PdfDoc *d;
    char *out;
    int cap, len;
    int legacy;
    float ctm[6], tm[6], tlm[6];
    float stack[16][6];
    int sp;
    float tc, tw, th, tl, fs;
    Font *font;
    Font *fonts[MAX_FONTS];
    int nfonts;
    int have_last;
    float lx, ly, lsize;
    PdfPageInfo *info;
    uint32_t best_area;
} TS;

static uint32_t utf16_first(const uint8_t *s, int n, uint32_t *second)
{
    uint32_t cps[2] = {0, 0};
    int k = 0;
    for (int i = 0; i + 1 < n && k < 2; i += 2) {
        uint32_t c = (uint32_t)(s[i] << 8 | s[i + 1]);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
            uint32_t lo = (uint32_t)(s[i + 2] << 8 | s[i + 3]);
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        cps[k++] = c;
    }
    if (n == 1)
        cps[0] = s[0];
    *second = cps[1];
    return cps[0];
}

static unsigned str_code(const Obj *o)
{
    unsigned v = 0;
    for (int i = 0; i < o->n && i < 2; i++)
        v = v << 8 | (uint8_t)o->u.s[i];
    return v;
}

static void add_urange(Font *f, unsigned lo, unsigned hi, uint32_t cp, uint32_t cp2)
{
    if (hi < lo || hi - lo > 0xFFFF || (cp == 0 && lo == hi)) /* a range may start at 0: identity maps */
        return;
    if (f->nu == f->cu) {
        int nc = f->cu ? f->cu * 2 : 64;
        if (nc > 65536) return;
        URange *nu = realloc(f->u, sizeof(URange) * nc);
        if (!nu) return;
        f->u = nu;
        f->cu = nc;
    }
    f->u[f->nu++] = (URange){(uint16_t)lo, (uint16_t)hi, cp, lo == hi ? cp2 : 0};
}

static int cmp_urange(const void *a, const void *b)
{
    return (int)((const URange *)a)->lo - (int)((const URange *)b)->lo;
}

static void parse_cmap(PdfDoc *d, Font *f, const uint8_t *s, int n)
{
    Lex L = {s, s + n, d, 0};
    int mode = 0, two = -1;
    Obj op[3];
    int nop = 0;
    size_t mark = d->ar_used;
    while (L.p < L.end) {
        Obj v;
        if (parse_value(&L, &v, 0) < 0) {
            if (L.p >= L.end) break;
            continue;
        }
        if (v.t == O_KW) {
            if (!strcmp(v.u.s, "begincodespacerange")) mode = 1;
            else if (!strcmp(v.u.s, "beginbfchar")) mode = 2;
            else if (!strcmp(v.u.s, "beginbfrange")) mode = 3;
            else if (!strncmp(v.u.s, "end", 3)) mode = 0;
            nop = 0;
            d->ar_used = mark;
            continue;
        }
        if (!mode) {
            d->ar_used = mark;
            continue;
        }
        op[nop++] = v;
        int need = mode == 3 ? 3 : 2;
        if (nop < need)
            continue;
        if (mode == 1) {
            if (op[0].t == O_STR && two < 0) two = op[0].n >= 2;
            if (op[0].t == O_STR && op[0].n >= 2) two = 1;
        } else if (mode == 2) {
            if (op[0].t == O_STR) {
                if (two < 0) two = op[0].n >= 2;
                uint32_t c2 = 0, c = 0;
                if (op[1].t == O_STR) c = utf16_first((const uint8_t *)op[1].u.s, op[1].n, &c2);
                else if (op[1].t == O_NAME) c = glyph_cp(op[1].u.s);
                add_urange(f, str_code(&op[0]), str_code(&op[0]), c, c2);
            }
        } else if (op[0].t == O_STR && op[1].t == O_STR) {
            if (two < 0) two = op[0].n >= 2;
            unsigned lo = str_code(&op[0]), hi = str_code(&op[1]);
            if (op[2].t == O_STR) {
                uint32_t c2;
                uint32_t c = utf16_first((const uint8_t *)op[2].u.s, op[2].n, &c2);
                if (lo == hi) add_urange(f, lo, hi, c, c2);
                else add_urange(f, lo, hi, c, 0);
            } else if (op[2].t == O_ARR) {
                for (int i = 0; i < op[2].n && lo + i <= hi; i++) {
                    Obj *e = &op[2].u.a[i];
                    if (e->t != O_STR) continue;
                    uint32_t c2, c = utf16_first((const uint8_t *)e->u.s, e->n, &c2);
                    add_urange(f, lo + i, lo + i, c, c2);
                }
            }
        }
        nop = 0;
        d->ar_used = mark;
    }
    d->ar_used = mark;
    if (two >= 0)
        f->two = two;
    if (f->nu)
        qsort(f->u, f->nu, sizeof(URange), cmp_urange);
}

static void add_wrange(Font *f, unsigned lo, unsigned hi, float w)
{
    if (hi < lo || f->ncw >= 20000)
        return;
    if (f->ncw == f->ccw) {
        int nc = f->ccw ? f->ccw * 2 : 64;
        WRange *n = realloc(f->cw, sizeof(WRange) * nc);
        if (!n) return;
        f->cw = n;
        f->ccw = nc;
    }
    f->cw[f->ncw++] = (WRange){(uint16_t)lo, (uint16_t)(hi > 0xFFFF ? 0xFFFF : hi), w};
}

static int cmp_wrange(const void *a, const void *b)
{
    return (int)((const WRange *)a)->lo - (int)((const WRange *)b)->lo;
}

static void build_font(TS *ts, Font *f, Obj *fd)
{
    PdfDoc *d = ts->d;
    Obj *sub = dgetr(d, fd, "Subtype");
    int type0 = is_name(sub, "Type0"), type3 = is_name(sub, "Type3");
    f->wscale = 1;
    f->dw = 1000;
    if (type0) {
        f->two = 1;
        Obj *df = dgetr(d, fd, "DescendantFonts");
        Obj *cid = df && df->t == O_ARR && df->n ? res(d, &df->u.a[0]) : NULL;
        float v;
        if (onum(dgetr(d, cid, "DW"), &v)) f->dw = v;
        Obj *w = dgetr(d, cid, "W");
        for (int i = 0; w && w->t == O_ARR && i < w->n;) {
            float a, b, c;
            if (!onum(res(d, &w->u.a[i]), &a)) break;
            Obj *nx = i + 1 < w->n ? res(d, &w->u.a[i + 1]) : NULL;
            if (nx && nx->t == O_ARR) {
                for (int k = 0; k < nx->n; k++)
                    if (onum(res(d, &nx->u.a[k]), &c)) add_wrange(f, (unsigned)a + k, (unsigned)a + k, c);
                i += 2;
            } else if (onum(nx, &b) && i + 2 < w->n && onum(res(d, &w->u.a[i + 2]), &c)) {
                add_wrange(f, (unsigned)a, (unsigned)b, c);
                i += 3;
            } else {
                break;
            }
        }
        if (f->ncw)
            qsort(f->cw, f->ncw, sizeof(WRange), cmp_wrange);
        Obj *enc = dgetr(d, fd, "Encoding");
        if (is_name(enc, "Identity-V") || is_name(enc, "Identity-H")) f->two = 1;
    } else {
        f->simple = 1;
        Obj *bf = dgetr(d, fd, "BaseFont");
        int symbol = bf && bf->t == O_NAME && strstr(bf->u.s, "Symbol");
        Obj *enc = dgetr(d, fd, "Encoding");
        Obj *base = enc && enc->t == O_DICT ? dgetr(d, enc, "BaseEncoding") : enc;
        int mac = is_name(base, "MacRomanEncoding"), std = is_name(base, "StandardEncoding");
        int named = is_name(base, "WinAnsiEncoding") || mac || std;
        for (int c = 0; c < 256; c++) {
            uint32_t cp;
            if (symbol && !named) cp = symbol_cp(c);
            else if (mac && c >= 128) cp = MACROMAN_80[c - 128];
            else if (!named && ts->legacy) cp = enc_cp(ENC_1253, (uint8_t)c);
            else cp = enc_cp(ENC_1252, (uint8_t)c);
            if (std && c == 0x27) cp = 0x2019;
            if (std && c == 0x60) cp = 0x2018;
            f->enc[c] = cp;
        }
        Obj *diff = enc && enc->t == O_DICT ? dgetr(d, enc, "Differences") : NULL;
        int code = 0;
        for (int i = 0; diff && diff->t == O_ARR && i < diff->n; i++) {
            Obj *e = res(d, &diff->u.a[i]);
            float v;
            if (onum(e, &v)) {
                code = (int)v;
            } else if (e && e->t == O_NAME) {
                if (code >= 0 && code < 256) {
                    uint32_t cp = glyph_cp(e->u.s);
                    if (cp || !symbol) f->enc[code] = cp;
                }
                code++;
            }
        }
        Obj *ws = dgetr(d, fd, "Widths");
        int fc = dint(d, fd, "FirstChar", 0);
        float miss = 0;
        Obj *desc = dgetr(d, fd, "FontDescriptor");
        onum(dgetr(d, desc, "MissingWidth"), &miss);
        for (int c = 0; c < 256; c++)
            f->w[c] = miss;
        if (ws && ws->t == O_ARR) {
            f->has_w = 1;
            for (int i = 0; i < ws->n; i++) {
                float v;
                if (fc + i >= 0 && fc + i < 256 && onum(res(d, &ws->u.a[i]), &v))
                    f->w[fc + i] = v;
            }
        }
        if (type3) {
            Obj *fm = dgetr(d, fd, "FontMatrix");
            float a;
            if (fm && fm->t == O_ARR && fm->n >= 1 && onum(&fm->u.a[0], &a))
                f->wscale = a * 1000;
        }
    }
    Obj *tu = dgetr(d, fd, "ToUnicode");
    if (tu && tu->stream) {
        int n;
        uint8_t *cm = stream_bytes(d, tu, &n);
        if (cm) {
            parse_cmap(d, f, cm, n);
            free(cm);
        }
    }
    if (type0)
        f->simple = 0;
}

static Font *find_font(TS *ts, Obj *resources, const char *name)
{
    PdfDoc *d = ts->d;
    Obj *fonts = dgetr(d, resources, "Font");
    Obj *ref = dget(fonts, name);
    int num = ref && ref->t == O_REF ? ref->n : -1;
    for (int i = 0; i < ts->nfonts; i++)
        if (num >= 0 ? ts->fonts[i]->num == num : (ts->fonts[i]->num < 0 && !strcmp(ts->fonts[i]->name, name)))
            return ts->fonts[i];
    Obj *fd = res(d, ref);
    if (!fd || fd->t != O_DICT)
        return NULL;
    Font *f = calloc(1, sizeof(Font));
    if (!f)
        return NULL;
    f->num = num;
    snprintf(f->name, sizeof(f->name), "%s", name);
    build_font(ts, f, fd);
    if (ts->nfonts == MAX_FONTS) { /* drop the oldest */
        free(ts->fonts[0]->u);
        free(ts->fonts[0]->cw);
        free(ts->fonts[0]);
        memmove(ts->fonts, ts->fonts + 1, sizeof(Font *) * (MAX_FONTS - 1));
        ts->nfonts--;
    }
    ts->fonts[ts->nfonts++] = f;
    return f;
}

static int font_cp(Font *f, unsigned code, uint32_t *cp, uint32_t *cp2)
{
    *cp2 = 0;
    if (f->nu) {
        int lo = 0, hi = f->nu - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            URange *r = &f->u[mid];
            if (code < r->lo) hi = mid - 1;
            else if (code > r->hi) lo = mid + 1;
            else {
                *cp = r->cp + (code - r->lo);
                *cp2 = r->cp2;
                return 1;
            }
        }
    }
    if (f->simple && code < 256 && f->enc[code]) {
        *cp = f->enc[code];
        return 1;
    }
    return 0;
}

static float font_w(Font *f, unsigned code)
{
    if (!f->simple && f->two) {
        int lo = 0, hi = f->ncw - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            WRange *r = &f->cw[mid];
            if (code < r->lo) hi = mid - 1;
            else if (code > r->hi) lo = mid + 1;
            else return r->w;
        }
        return f->dw;
    }
    if (code < 256 && f->has_w)
        return f->w[code] * f->wscale;
    return 500;
}

/* --------------------------------------------------------------- output */

static void out_raw(TS *ts, const char *s, int n)
{
    if (ts->len + n >= ts->cap)
        return;
    memcpy(ts->out + ts->len, s, n);
    ts->len += n;
}

static void out_cp(TS *ts, uint32_t cp)
{
    if (cp == 0 || cp == 0xAD || cp == 0xFEFF || cp == 0xFFFD)
        return;
    if (cp < 32 || cp == 0xA0 || cp == 0x7F)
        cp = ' ';
    switch (cp) { /* ligatures the font doesn't have */
    case 0xFB00: out_raw(ts, "ff", 2); return;
    case 0xFB01: out_raw(ts, "fi", 2); return;
    case 0xFB02: out_raw(ts, "fl", 2); return;
    case 0xFB03: out_raw(ts, "ffi", 3); return;
    case 0xFB04: out_raw(ts, "ffl", 3); return;
    }
    if (cp == ' ' && (ts->len == 0 || ts->out[ts->len - 1] == ' ' || ts->out[ts->len - 1] == '\n'))
        return;
    char b[4];
    out_raw(ts, b, utf8_put(cp, b));
}

static void out_nl(TS *ts, int para)
{
    while (ts->len && ts->out[ts->len - 1] == ' ')
        ts->len--;
    if (!ts->len)
        return;
    int have = 0;
    while (have < ts->len && ts->out[ts->len - 1 - have] == '\n')
        have++;
    for (int want = para ? 2 : 1; have < want; have++)
        out_raw(ts, "\n", 1);
}

/* ----------------------------------------------------------- text state */

static void mat_mul(const float *a, const float *b, float *o)
{
    float r[6];
    r[0] = a[0] * b[0] + a[1] * b[2];
    r[1] = a[0] * b[1] + a[1] * b[3];
    r[2] = a[2] * b[0] + a[3] * b[2];
    r[3] = a[2] * b[1] + a[3] * b[3];
    r[4] = a[4] * b[0] + a[5] * b[2] + b[4];
    r[5] = a[4] * b[1] + a[5] * b[3] + b[5];
    memcpy(o, r, sizeof(r));
}

static void mat_id(float *m)
{
    m[0] = m[3] = 1;
    m[1] = m[2] = m[4] = m[5] = 0;
}

static float run_size(TS *ts, float *x, float *y)
{
    float m[6];
    mat_mul(ts->tm, ts->ctm, m);
    *x = m[4];
    *y = m[5];
    float s = ts->fs * sqrtf(m[2] * m[2] + m[3] * m[3]);
    if (s < 0) s = -s;
    return s > 0.01f ? s : 1;
}

static void run_begin(TS *ts)
{
    float x, y, sz = run_size(ts, &x, &y);
    if (ts->have_last) {
        float ref = sz > ts->lsize ? sz : ts->lsize;
        float dy = y - ts->ly, dx = x - ts->lx;
        if (dy > 0.5f * ref || dy < -0.5f * ref)
            out_nl(ts, dy < -2.1f * ref);
        else if (dx > 0.18f * ref || dx < -1.5f * ref)
            out_cp(ts, ' ');
    }
}

static void run_end(TS *ts)
{
    float x, y;
    ts->lsize = run_size(ts, &x, &y);
    ts->lx = x;
    ts->ly = y;
    ts->have_last = 1;
}

static void advance(TS *ts, float tx)
{
    ts->tm[4] += tx * ts->tm[0];
    ts->tm[5] += tx * ts->tm[1];
}

static void show(TS *ts, const Obj *s)
{
    if (!s || s->t != O_STR)
        return;
    Font *f = ts->font;
    run_begin(ts);
    const uint8_t *p = (const uint8_t *)s->u.s;
    int two = f && f->two;
    for (int i = 0; i + (two ? 1 : 0) < s->n;) {
        unsigned code = two ? (unsigned)(p[i] << 8 | p[i + 1]) : p[i];
        i += two ? 2 : 1;
        uint32_t cp = 0, cp2 = 0;
        int ok = f ? font_cp(f, code, &cp, &cp2) : (cp = enc_cp(ts->legacy ? ENC_1253 : ENC_1252, (uint8_t)code)) != 0;
        if (ok) {
            out_cp(ts, cp);
            if (cp2) out_cp(ts, cp2);
        } else if (ts->info) {
            ts->info->undecodable++;
        }
        float w = f ? font_w(f, code) : 500;
        float tx = (w / 1000 * ts->fs + ts->tc + (!two && code == 32 ? ts->tw : 0)) * ts->th;
        advance(ts, tx);
    }
    run_end(ts);
}

static void next_line(TS *ts, float tx, float ty)
{
    float t[6] = {1, 0, 0, 1, tx, ty};
    mat_mul(t, ts->tlm, ts->tlm);
    memcpy(ts->tm, ts->tlm, sizeof(ts->tm));
}

static int nums(Obj *ops, int nops, float *v, int n)
{
    if (nops < n)
        return 0;
    for (int i = 0; i < n; i++)
        if (!onum(&ops[nops - n + i], &v[i]))
            return 0;
    return 1;
}

static void run_content(TS *ts, const uint8_t *s, int n, Obj *resources, int depth);

static int has_filter(PdfDoc *d, Obj *o, const char *name)
{
    Obj *f = dgetr(d, o, "Filter");
    if (is_name(f, name))
        return 1;
    for (int i = 0; f && f->t == O_ARR && i < f->n; i++)
        if (is_name(res(d, &f->u.a[i]), name))
            return 1;
    return 0;
}

static void do_xobject(TS *ts, Obj *resources, const char *name, int depth)
{
    PdfDoc *d = ts->d;
    Obj *xo = dgetr(d, dgetr(d, resources, "XObject"), name);
    if (!xo || !xo->stream)
        return;
    Obj *sub = dgetr(d, xo, "Subtype");
    if (is_name(sub, "Image")) {
        if (!ts->info || !has_filter(d, xo, "DCTDecode"))
            return;
        int w = dint(d, xo, "Width", 0), h = dint(d, xo, "Height", 0);
        uint32_t area = (uint32_t)(w > 0 ? w : 0) * (uint32_t)(h > 0 ? h : 0);
        if (area > ts->best_area) {
            int len = stream_len(d, xo);
            if (len > 0) {
                ts->best_area = area;
                ts->info->img_off = xo->soff;
                ts->info->img_len = (uint32_t)len;
                ts->info->img_w = w;
                ts->info->img_h = h;
            }
        }
        return;
    }
    if (!is_name(sub, "Form") || depth >= 4)
        return;
    int len;
    uint8_t *data = stream_bytes(d, xo, &len);
    if (!data)
        return;
    Obj *r = dgetr(d, xo, "Resources");
    float saved[6];
    memcpy(saved, ts->ctm, sizeof(saved));
    Obj *m = dgetr(d, xo, "Matrix");
    if (m && m->t == O_ARR && m->n == 6) {
        float mm[6];
        int ok = 1;
        for (int i = 0; i < 6; i++)
            ok &= onum(&m->u.a[i], &mm[i]);
        if (ok)
            mat_mul(mm, ts->ctm, ts->ctm);
    }
    run_content(ts, data, len, r ? r : resources, depth + 1);
    memcpy(ts->ctm, saved, sizeof(saved));
    free(data);
}

/* Skips an inline image: BI <dict> ID <binary> EI. */
static void skip_inline_image(Lex *L)
{
    while (L->p < L->end) {
        Obj v;
        if (parse_value(L, &v, 0) < 0) {
            if (L->p >= L->end) return;
            continue;
        }
        if (is_kw(&v, "ID"))
            break;
    }
    if (L->p < L->end)
        L->p++;
    while (L->p + 2 <= L->end) {
        if (L->p[0] == 'E' && L->p[1] == 'I' && (L->p + 2 == L->end || is_ws(L->p[2])) &&
            is_ws(L->p[-1])) {
            L->p += 2;
            return;
        }
        L->p++;
    }
    L->p = L->end;
}

static void run_content(TS *ts, const uint8_t *s, int n, Obj *resources, int depth)
{
    PdfDoc *d = ts->d;
    Lex L = {s, s + n, d, 0};
    Obj ops[MAX_OPS];
    int nops = 0;
    size_t mark = d->ar_used;
    float v[6];
    while (L.p < L.end && ts->len < ts->cap - 8) {
        Obj o;
        if (parse_value(&L, &o, 0) < 0) { /* the operands so far are no use */
            d->ar_used = mark;
            nops = 0;
            continue;
        }
        if (o.t != O_KW) {
            if (nops < MAX_OPS) ops[nops++] = o;
            continue;
        }
        const char *k = o.u.s;
        char nm[64] = "";
        if (!strcmp(k, "Tj")) {
            if (nops) show(ts, &ops[nops - 1]);
        } else if (!strcmp(k, "TJ")) {
            Obj *a = nops ? &ops[nops - 1] : NULL;
            for (int i = 0; a && a->t == O_ARR && i < a->n; i++) {
                Obj *e = &a->u.a[i];
                if (e->t == O_STR) {
                    show(ts, e);
                } else if (e->t == O_NUM) {
                    advance(ts, -e->num / 1000 * ts->fs * ts->th);
                    if (e->num < -180)
                        out_cp(ts, ' ');
                }
            }
        } else if (!strcmp(k, "'")) {
            next_line(ts, 0, -ts->tl);
            if (nops) show(ts, &ops[nops - 1]);
        } else if (!strcmp(k, "\"")) {
            if (nops >= 3) {
                onum(&ops[nops - 3], &ts->tw);
                onum(&ops[nops - 2], &ts->tc);
            }
            next_line(ts, 0, -ts->tl);
            if (nops) show(ts, &ops[nops - 1]);
        } else if (!strcmp(k, "Td")) {
            if (nums(ops, nops, v, 2)) next_line(ts, v[0], v[1]);
        } else if (!strcmp(k, "TD")) {
            if (nums(ops, nops, v, 2)) {
                ts->tl = -v[1];
                next_line(ts, v[0], v[1]);
            }
        } else if (!strcmp(k, "T*")) {
            next_line(ts, 0, -ts->tl);
        } else if (!strcmp(k, "Tm")) {
            if (nums(ops, nops, v, 6)) {
                memcpy(ts->tm, v, sizeof(ts->tm));
                memcpy(ts->tlm, v, sizeof(ts->tlm));
            }
        } else if (!strcmp(k, "BT")) {
            mat_id(ts->tm);
            mat_id(ts->tlm);
        } else if (!strcmp(k, "Tc")) {
            if (nums(ops, nops, v, 1)) ts->tc = v[0];
        } else if (!strcmp(k, "Tw")) {
            if (nums(ops, nops, v, 1)) ts->tw = v[0];
        } else if (!strcmp(k, "Tz")) {
            if (nums(ops, nops, v, 1)) ts->th = v[0] / 100;
        } else if (!strcmp(k, "TL")) {
            if (nums(ops, nops, v, 1)) ts->tl = v[0];
        } else if (!strcmp(k, "q")) {
            if (ts->sp < 16) memcpy(ts->stack[ts->sp++], ts->ctm, sizeof(ts->ctm));
        } else if (!strcmp(k, "Q")) {
            if (ts->sp > 0) memcpy(ts->ctm, ts->stack[--ts->sp], sizeof(ts->ctm));
        } else if (!strcmp(k, "cm")) {
            if (nums(ops, nops, v, 6)) mat_mul(v, ts->ctm, ts->ctm);
        } else if (!strcmp(k, "BI")) {
            skip_inline_image(&L);
        } else if ((!strcmp(k, "Tf") && nops >= 2 && ops[nops - 2].t == O_NAME) ||
                   (!strcmp(k, "Do") && nops >= 1 && ops[nops - 1].t == O_NAME)) {
            /* These load objects: copy the operands out, release them, then load. */
            int tf = k[0] == 'T';
            snprintf(nm, sizeof(nm), "%s", ops[nops - (tf ? 2 : 1)].u.s);
            float size = 0;
            if (tf) onum(&ops[nops - 1], &size);
            d->ar_used = mark;
            nops = 0;
            if (tf) {
                ts->font = find_font(ts, resources, nm);
                ts->fs = size;
            } else {
                do_xobject(ts, resources, nm, depth);
            }
            mark = d->ar_used; /* what was just loaded stays */
            continue;
        }
        nops = 0;
        d->ar_used = mark;
    }
    d->ar_used = mark;
}

int pdf_page_text(PdfDoc *d, int page, int legacy_greek, char *out, int cap, PdfPageInfo *info)
{
    if (info)
        memset(info, 0, sizeof(*info));
    if (!d || page < 0 || page >= d->npages || cap < 2)
        return PDF_E_FORMAT;
    areset(d);
    TS *ts = calloc(1, sizeof(TS));
    if (!ts)
        return PDF_E_MEMORY;
    ts->d = d;
    ts->out = out;
    ts->cap = cap;
    ts->legacy = legacy_greek;
    ts->th = 1;
    ts->info = info;
    mat_id(ts->ctm);
    mat_id(ts->tm);
    mat_id(ts->tlm);
    Obj *pg = load_obj(d, d->pages[page]);
    Obj *owner = d->res_owner[page] ? load_obj(d, d->res_owner[page]) : NULL;
    Obj *resources = dgetr(d, owner, "Resources");
    Obj *c = dgetr(d, pg, "Contents");
    int nc = !c ? 0 : c->t == O_ARR ? c->n : 1;
    /* Concatenate the content streams: an operator may be split between two. */
    uint8_t *all = NULL;
    int total = 0;
    for (int i = 0; i < nc; i++) {
        Obj *s = c->t == O_ARR ? res(d, &c->u.a[i]) : c;
        int len;
        uint8_t *data = stream_bytes(d, s, &len);
        if (!data)
            continue;
        if (total + len + 1 <= MAX_STREAM) {
            uint8_t *na = realloc(all, (size_t)total + len + 1);
            if (na) {
                all = na;
                memcpy(all + total, data, len);
                total += len;
                all[total++] = '\n';
            }
        }
        free(data);
    }
    if (all)
        run_content(ts, all, total, resources, 0);
    free(all);
    while (ts->len && (ts->out[ts->len - 1] == ' ' || ts->out[ts->len - 1] == '\n'))
        ts->len--;
    out[ts->len] = 0;
    int len = ts->len;
    for (int i = 0; i < ts->nfonts; i++) {
        free(ts->fonts[i]->u);
        free(ts->fonts[i]->cw);
        free(ts->fonts[i]);
    }
    free(ts);
    areset(d);
    return len;
}
