/* XLT - reader for text, hex and PDF. See reader.h.
 *
 * Everything that touches the file runs on the background worker (bg.c):
 * loading and converting the text, wrapping it into lines, opening the PDF
 * and extracting a page, decoding a scanned page's JPEG. A job hands back
 * freshly allocated buffers; the UI thread adopts them and gives the old
 * ones back to the worker to free, so a job never reads memory that was
 * freed under it (jobs run in order).
 *
 *   ↑/↓ line, ←/→ screen, ✕ next screen (then next page), L/R previous /
 *   next page (PDF) or start / end, □ find next, △ menu, ○ back.
 */
#include <pspkernel.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "reader.h"
#include "ui_internal.h"
#include "anim.h"
#include "bg.h"
#include "imgdecode.h"
#include "library.h"
#include "menu.h"
#include "osk.h"
#include "pdf.h"
#include "textenc.h"

#define TOP 28
#define BOTTOM 244
#define FOOTER_Y 248
#define TEXT_X 12
#define TEXT_W (SCREEN_W - 2 * TEXT_X - 6)
#define TEXT_MAX (2 << 20)
#define HEX_MAX (4 << 20)
#define PAGE_CAP (256 << 10)
#define IMG_W 480
#define IMG_MAX_H 1400
#define IMG_VIEW_H (BOTTOM - TOP + 4)
#define TBW 512
#define MAX_LINES 400000
#define MARKS 64

enum { J_LOAD, J_WRAP, J_PDF_OPEN, J_PDF_PAGE, J_PDF_FIND };

typedef struct {
    int gen, kind;
    int mode, enc, font, width;
    int page, legacy, want_img;
    int find_from; /* J_PDF_FIND: byte offset in `page` to search after */
    const char *text; /* J_WRAP: the text being shown (owned by the UI, freed only through the worker) */
    int text_len;
    char path[256];
    char query[OSK_MAX + 1];
} Job;

typedef struct {
    int gen, kind, ok;
    char err[96];
    int mode, enc_used, truncated;
    int64_t file_size;
    char *text;
    int len;
    int *lines;
    int nlines;
    uint8_t *bytes;
    int nbytes;
    int pages, page;
    PdfPageInfo info;
    uint8_t *img;
    int iw, ih;
    int found_page, found_off; /* -1: not found */
    int mark_page, mark_off;   /* saved reading position, -1 if none */
} Result;

static volatile int s_ready;
static Result s_res;
static char s_marks_path[256];

/* Worker-thread side: the open PDF. */
static PdfDoc *s_doc;
static char s_doc_path[256];

static struct {
    int open;
    Spring t;
    char path[256], name[128];
    int mode; /* READER_TEXT / HEX / PDF */
    int enc, enc_used, font, legacy;
    int loading;
    char err[96];
    int gen;
    char *text;
    int len, truncated;
    int64_t file_size;
    int *lines;
    int nlines;
    uint8_t *bytes;
    int nbytes;
    int top;
    int pages, page;
    PdfPageInfo info;
    int show_img;
    uint8_t *img;
    int iw, ih, img_top, img_dirty;
    char query[OSK_MAX + 1];
    int hl_off, hl_len; /* the last match, highlighted */
    int pending_jump;   /* byte offset to show once the page arrives, -1 if none */
    int use_mark;       /* the first load goes back to the saved position */
} R;

/* ------------------------------------------------------------ positions */

typedef struct { int page, off; char path[256]; } Mark;

static int load_marks(Mark *m)
{
    int n = 0;
    SceUID fd = sceIoOpen(s_marks_path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return 0;
    static char buf[MARKS * 280];
    int len = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (len <= 0)
        return 0;
    buf[len] = 0;
    for (char *line = buf; *line && n < MARKS;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        int pg, off, used = 0;
        if (sscanf(line, "%d %d %n", &pg, &off, &used) == 2 && used && line[used]) {
            m[n].page = pg;
            m[n].off = off;
            snprintf(m[n].path, sizeof(m[n].path), "%s", line + used);
            n++;
        }
        if (!nl) break;
        line = nl + 1;
    }
    return n;
}

static void find_mark(const char *path, int *page, int *off)
{
    static Mark m[MARKS];
    *page = *off = -1;
    int n = load_marks(m);
    for (int i = 0; i < n; i++)
        if (!strcmp(m[i].path, path)) {
            *page = m[i].page;
            *off = m[i].off;
            return;
        }
}

typedef struct { int page, off; char path[256]; } MarkArg;

static void job_save_mark(void *arg)
{
    const MarkArg *a = arg;
    static Mark m[MARKS];
    int n = load_marks(m);
    SceUID fd = sceIoOpen(s_marks_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return;
    char line[300];
    int l = snprintf(line, sizeof(line), "%d %d %s\n", a->page, a->off, a->path);
    sceIoWrite(fd, line, l);
    for (int i = 0, kept = 1; i < n && kept < MARKS; i++) { /* the newest first */
        if (!strcmp(m[i].path, a->path))
            continue;
        l = snprintf(line, sizeof(line), "%d %d %s\n", m[i].page, m[i].off, m[i].path);
        sceIoWrite(fd, line, l);
        kept++;
    }
    sceIoClose(fd);
}

void reader_init(const char *app_dir)
{
    snprintf(s_marks_path, sizeof(s_marks_path), "%scache/reader.txt", app_dir);
}

/* --------------------------------------------------------------- worker */

static void post_result(Result *r)
{
    while (s_ready)
        sceKernelDelayThread(5000); /* the UI adopts a result every frame */
    s_res = *r;
    __sync_synchronize();
    s_ready = 1;
}

/* Line starts for text wrapped into width (pixels) with font. */
static int wrap(const char *t, int len, int font, int width, int **out)
{
    int cap = 1024, n = 0;
    int *lines = malloc(sizeof(int) * cap);
    if (!lines)
        return -1;
    int start = 0, w = 0, last_space = -1, i = 0;
#define PUSH(v) do { \
        if (n == cap) { \
            if (cap >= MAX_LINES) goto done; \
            int *nl_ = realloc(lines, sizeof(int) * cap * 2); \
            if (!nl_) goto done; \
            lines = nl_; cap *= 2; \
        } \
        lines[n++] = (v); \
    } while (0)
    while (i < len) {
        char c = t[i];
        if (c == '\n') {
            PUSH(start);
            start = ++i;
            w = 0;
            last_space = -1;
            continue;
        }
        const char *p = t + i;
        int adv = gfx_glyph_adv(font, &p);
        int nb = (int)(p - (t + i));
        if (w + adv > width && i > start) {
            PUSH(start);
            if (last_space > start) {
                start = last_space;
                w = 0;
                for (const char *q = t + start; q < t + i;)
                    w += gfx_glyph_adv(font, &q);
            } else {
                start = i;
                w = 0;
            }
            last_space = -1;
            continue;
        }
        if (c == ' ')
            last_space = i + 1;
        w += adv;
        i += nb;
    }
    PUSH(start);
done:
#undef PUSH
    *out = lines;
    return n;
}

static int looks_binary(const uint8_t *s, int n)
{
    if (n >= 2 && ((s[0] == 0xFF && s[1] == 0xFE) || (s[0] == 0xFE && s[1] == 0xFF)))
        return 0; /* UTF-16 */
    int bad = 0, m = n < 4096 ? n : 4096;
    for (int i = 0; i < m; i++) {
        if (s[i] == 0)
            return 1;
        if (s[i] < 32 && s[i] != '\n' && s[i] != '\r' && s[i] != '\t' && s[i] != 12 && s[i] != 27)
            bad++;
    }
    return m && bad * 20 > m;
}

static void set_err(Result *r, const char *msg)
{
    r->ok = 0;
    snprintf(r->err, sizeof(r->err), "%s", msg);
}

static void do_load(const Job *j, Result *r)
{
    SceUID fd = sceIoOpen(j->path, PSP_O_RDONLY, 0);
    if (fd < 0) {
        set_err(r, "Can't open this file");
        return;
    }
    int64_t size = sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoLseek(fd, 0, PSP_SEEK_SET);
    r->file_size = size;
    int mode = j->mode;
    int cap = mode == READER_HEX ? HEX_MAX : TEXT_MAX;
    int want = size > cap ? cap : (int)size;
    uint8_t *buf = malloc(want + 1);
    if (!buf) {
        sceIoClose(fd);
        set_err(r, "Not enough memory for this file");
        return;
    }
    int n = want > 0 ? sceIoRead(fd, buf, want) : 0;
    sceIoClose(fd);
    if (n < 0) n = 0;
    if (mode == READER_AUTO)
        mode = looks_binary(buf, n) ? READER_HEX : READER_TEXT;
    r->mode = mode;
    r->truncated = size > n;
    if (mode == READER_HEX) {
        r->bytes = buf;
        r->nbytes = n;
        r->nlines = (n + 15) / 16;
        r->ok = 1;
        return;
    }
    int len, used;
    char *t = text_to_utf8(buf, n, j->enc, &len, &used);
    free(buf);
    if (!t) {
        set_err(r, "Not enough memory for this file");
        return;
    }
    r->text = t;
    r->len = len;
    r->enc_used = used;
    r->nlines = wrap(t, len, j->font, j->width, &r->lines);
    if (r->nlines < 0) {
        set_err(r, "Not enough memory for this file");
        return;
    }
    find_mark(j->path, &r->mark_page, &r->mark_off);
    r->ok = 1;
}

static int ensure_doc(const char *path, Result *r)
{
    if (s_doc && !strcmp(s_doc_path, path))
        return 0;
    pdf_close(s_doc);
    s_doc = NULL;
    int err = 0;
    power_boost();
    s_doc = pdf_open(path, &err);
    if (!s_doc) {
        set_err(r, err == PDF_E_ENCRYPTED ? "This PDF is encrypted: it can't be opened here"
                 : err == PDF_E_READ ? "Can't read this file"
                 : err == PDF_E_MEMORY ? "Not enough memory for this PDF"
                 : "This PDF is damaged or not supported");
        return -1;
    }
    snprintf(s_doc_path, sizeof(s_doc_path), "%s", path);
    return 0;
}

static void do_page(const Job *j, Result *r, int page)
{
    r->mode = READER_PDF;
    r->pages = pdf_page_count(s_doc);
    if (page >= r->pages) page = r->pages - 1;
    if (page < 0) page = 0;
    r->page = page;
    char *buf = malloc(PAGE_CAP);
    if (!buf) {
        set_err(r, "Not enough memory for this page");
        return;
    }
    int n = pdf_page_text(s_doc, page, j->legacy, buf, PAGE_CAP, &r->info);
    if (n < 0) {
        free(buf);
        set_err(r, "Can't read this page");
        return;
    }
    char *t = realloc(buf, n + 1);
    r->text = t ? t : buf;
    r->len = n;
    r->nlines = wrap(r->text, n, j->font, j->width, &r->lines);
    if (r->nlines < 0) r->nlines = 0;
    if (r->info.img_off && (j->want_img || n == 0)) {
        uint8_t *img = malloc((size_t)IMG_W * IMG_MAX_H * 4);
        int w = 0, h = 0;
        if (img && img_decode_at(j->path, r->info.img_off, IMG_FIT, IMG_W, IMG_MAX_H, img, &w, &h) == 0) {
            r->img = img;
            r->iw = w;
            r->ih = h;
        } else {
            free(img);
        }
    }
    r->ok = 1;
}

static void job_run(void *arg)
{
    const Job *j = arg;
    Result r;
    memset(&r, 0, sizeof(r));
    r.gen = j->gen;
    r.kind = j->kind;
    r.found_page = r.found_off = r.mark_page = r.mark_off = -1;
    power_boost();
    switch (j->kind) {
    case J_LOAD:
        do_load(j, &r);
        break;
    case J_WRAP:
        r.nlines = wrap(j->text, j->text_len, j->font, j->width, &r.lines);
        r.ok = r.nlines >= 0;
        break;
    case J_PDF_OPEN:
        if (ensure_doc(j->path, &r) == 0) {
            find_mark(j->path, &r.mark_page, &r.mark_off);
            do_page(j, &r, r.mark_page > 0 ? r.mark_page : 0);
        }
        break;
    case J_PDF_PAGE:
        if (ensure_doc(j->path, &r) == 0)
            do_page(j, &r, j->page);
        break;
    case J_PDF_FIND:
        if (ensure_doc(j->path, &r) == 0) {
            int np = pdf_page_count(s_doc);
            char *buf = malloc(PAGE_CAP);
            for (int k = 0; buf && k <= np; k++) {
                int pg = (j->page + k) % np;
                int n = pdf_page_text(s_doc, pg, j->legacy, buf, PAGE_CAP, NULL);
                int from = k == 0 ? j->find_from : 0;
                int at = n > 0 ? text_find(buf, n, from, j->query) : -1;
                if (at >= 0) {
                    r.found_page = pg;
                    r.found_off = at;
                    break;
                }
            }
            free(buf);
            r.ok = 1;
        }
        break;
    }
    if (!library_scanning())
        power_idle();
    post_result(&r);
}

typedef struct { void *a, *b, *c, *d; } FreeArg;

static void job_free(void *arg)
{
    const FreeArg *f = arg;
    free(f->a);
    free(f->b);
    free(f->c);
    free(f->d);
}

static void job_close_doc(void *arg)
{
    pdf_close(s_doc);
    s_doc = NULL;
    s_doc_path[0] = 0;
}

static void free_later(void *a, void *b, void *c, void *d)
{
    if (!a && !b && !c && !d)
        return;
    FreeArg f = {a, b, c, d};
    bg_post(job_free, &f, sizeof(f));
}

/* ------------------------------------------------------------------- UI */

static int line_h(void) { return gfx_line_h(R.font); }
static int rows(void) { return (BOTTOM - TOP) / (R.mode == READER_HEX ? gfx_line_h(FONT_SMALL) : line_h()); }
static int total_rows(void) { return R.mode == READER_HEX ? (R.nbytes + 15) / 16 : R.nlines; }
static int showing_img(void) { return R.mode == READER_PDF && R.img && (R.show_img || R.len == 0); }

static void post_job(int kind, int page, int want_img)
{
    static Job j;
    memset(&j, 0, sizeof(j));
    j.gen = ++R.gen;
    j.kind = kind;
    j.mode = R.mode == READER_PDF ? READER_AUTO : R.mode;
    j.enc = R.enc;
    j.font = R.font;
    j.width = TEXT_W;
    j.page = page;
    j.legacy = R.legacy;
    j.want_img = want_img;
    j.text = R.text;
    j.text_len = R.len;
    snprintf(j.path, sizeof(j.path), "%s", R.path);
    snprintf(j.query, sizeof(j.query), "%s", R.query);
    if (kind == J_PDF_FIND) {
        j.page = R.page;
        j.find_from = R.hl_len ? R.hl_off + 1 : R.top < R.nlines ? R.lines[R.top] : 0;
    }
    if (kind != J_WRAP)
        R.loading = 1;
    bg_post(job_run, &j, sizeof(j));
}

static void drop_doc_buffers(void)
{
    free_later(R.text, R.lines, R.bytes, R.img);
    R.text = NULL;
    R.lines = NULL;
    R.bytes = NULL;
    R.img = NULL;
    R.len = R.nlines = R.nbytes = 0;
}

static void save_mark(void)
{
    if (R.mode == READER_HEX || R.loading || R.err[0])
        return;
    MarkArg a;
    a.page = R.mode == READER_PDF ? R.page : 0;
    a.off = R.top < R.nlines && R.lines ? R.lines[R.top] : 0;
    snprintf(a.path, sizeof(a.path), "%s", R.path);
    bg_post(job_save_mark, &a, sizeof(a));
}

void reader_open(const char *path, int mode)
{
    if (R.open)
        return;
    drop_doc_buffers();
    snprintf(R.path, sizeof(R.path), "%s", path);
    const char *slash = strrchr(path, '/');
    snprintf(R.name, sizeof(R.name), "%s", slash ? slash + 1 : path);
    const char *dot = strrchr(R.name, '.');
    if (mode == READER_AUTO && dot && !strcasecmp(dot, ".pdf"))
        mode = READER_PDF;
    R.mode = mode == READER_PDF ? READER_PDF : mode;
    R.err[0] = 0;
    R.top = R.page = R.pages = 0;
    R.show_img = 0;
    R.img_top = 0;
    R.hl_len = 0;
    R.pending_jump = -1;
    R.use_mark = 1;
    memset(&R.info, 0, sizeof(R.info));
    if (!R.font) R.font = FONT_SMALL;
    R.open = 1;
    spring_to(&R.t, FX(1));
    post_job(mode == READER_PDF ? J_PDF_OPEN : J_LOAD, 0, 0);
    if (mode != READER_PDF)
        R.mode = mode; /* AUTO until the worker has looked at the bytes */
}

int reader_is_open(void) { return R.open || R.t.x > 0; }
int reader_covers(void) { return R.open && R.t.x >= FX(1); }

static void close_reader(void)
{
    save_mark();
    R.open = 0;
    R.gen++; /* anything still coming is dropped */
    spring_to(&R.t, 0);
    if (R.mode == READER_PDF)
        bg_post(job_close_doc, NULL, 0);
}

void reader_shutdown(void)
{
    if (R.open)
        close_reader();
    bg_post(job_close_doc, NULL, 0);
    bg_flush();
    spring_snap(&R.t, 0);
}

static int line_of(int off)
{
    int lo = 0, hi = R.nlines - 1, ans = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (R.lines[mid] <= off) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}

static void clamp_top(void)
{
    int max = total_rows() - rows();
    if (R.top > max) R.top = max;
    if (R.top < 0) R.top = 0;
}

static void upload_img(void)
{
    if (!R.img)
        return;
    int max = R.ih - IMG_VIEW_H;
    if (R.img_top > max) R.img_top = max;
    if (R.img_top < 0) R.img_top = 0;
    u16 *dst = (u16 *)(((u32)sceGeEdramGetAddr() + GFX_VRAM_VIEWER) | 0x40000000u);
    int h = R.ih < IMG_VIEW_H ? R.ih : IMG_VIEW_H;
    for (int y = 0; y < h; y++) {
        const uint8_t *s = R.img + (size_t)(R.img_top + y) * IMG_W * 4;
        u16 *d = dst + y * TBW;
        for (int x = 0; x < R.iw; x++, s += 4)
            d[x] = (u16)((s[2] >> 3) << 11 | (s[1] >> 2) << 5 | (s[0] >> 3));
    }
    R.img_dirty = 0;
}

static void go_page(int p)
{
    if (R.mode != READER_PDF || R.loading || p < 0 || p >= R.pages)
        return;
    R.hl_len = 0;
    post_job(J_PDF_PAGE, p, R.show_img);
}

static void find_next(void)
{
    if (!R.query[0])
        return;
    if (R.mode == READER_PDF) {
        if (!R.loading)
            post_job(J_PDF_FIND, 0, 0);
        return;
    }
    if (R.mode != READER_TEXT || !R.text)
        return;
    int from = R.hl_len ? R.hl_off + 1 : R.top < R.nlines ? R.lines[R.top] : 0;
    int at = text_find(R.text, R.len, from, R.query);
    if (at < 0 && from > 0)
        at = text_find(R.text, R.len, 0, R.query); /* wrap around */
    if (at < 0) {
        ui_toast("Not found");
        R.hl_len = 0;
        return;
    }
    R.hl_off = at;
    R.hl_len = (int)strlen(R.query); /* close enough: folding keeps byte lengths for most letters */
    R.top = line_of(at) - 2;
    clamp_top();
}

static void on_find(const char *text)
{
    if (!text)
        return;
    snprintf(R.query, sizeof(R.query), "%s", text);
    R.hl_len = 0;
    find_next();
}

static void on_goto(const char *text)
{
    if (!text)
        return;
    int p = atoi(text) - 1;
    if (p < 0 || p >= R.pages) {
        ui_toast("There's no such page");
        return;
    }
    go_page(p);
}

enum { M_FIND, M_FIND_NEXT, M_GOTO, M_ENC, M_LEGACY, M_SIZE, M_IMAGE, M_HEX, M_START, M_END };

static void fill_menu(MenuItem *it, int *n);

static void menu_cb(int id, int dir)
{
    if (R.loading)
        return; /* one job at a time: wait for the page */
    switch (id) {
    case M_FIND:
        menu_close();
        osk_open("Find", R.query, OSK_TEXT, on_find);
        return;
    case M_FIND_NEXT:
        menu_close();
        find_next();
        return;
    case M_GOTO:
        menu_close();
        osk_open("Go to page", "", OSK_NUMBER, on_goto);
        return;
    case M_ENC:
        R.enc = (R.enc + (dir < 0 ? ENC_COUNT - 1 : 1)) % ENC_COUNT;
        drop_doc_buffers();
        R.top = 0;
        post_job(J_LOAD, 0, 0);
        break;
    case M_LEGACY:
        R.legacy = !R.legacy;
        post_job(J_PDF_PAGE, R.page, R.show_img);
        break;
    case M_SIZE: {
        int off = R.top < R.nlines && R.lines ? R.lines[R.top] : 0;
        R.font = R.font == FONT_SMALL ? FONT_BODY : FONT_SMALL;
        if (R.text) {
            R.pending_jump = off;
            post_job(J_WRAP, 0, 0);
        }
        break;
    }
    case M_IMAGE:
        menu_close();
        R.show_img = !R.show_img;
        if (R.show_img && !R.img)
            post_job(J_PDF_PAGE, R.page, 1);
        else
            R.img_dirty = 1;
        return;
    case M_HEX:
        menu_close();
        drop_doc_buffers();
        R.mode = R.mode == READER_HEX ? READER_TEXT : READER_HEX;
        R.top = 0;
        post_job(J_LOAD, 0, 0);
        return;
    case M_START:
        menu_close();
        R.top = 0;
        R.img_top = 0;
        R.img_dirty = 1;
        return;
    case M_END:
        menu_close();
        R.top = total_rows();
        clamp_top();
        R.img_top = R.ih;
        R.img_dirty = 1;
        return;
    }
    int n;
    MenuItem *items = menu_items(&n);
    fill_menu(items, &n);
    menu_set_count(n);
}

static void fill_menu(MenuItem *it, int *n)
{
    int k = 0;
#define ADD(i, l) do { memset(&it[k], 0, sizeof(MenuItem)); it[k].id = (i); \
        snprintf(it[k].label, sizeof(it[k].label), "%s", (l)); k++; } while (0)
    if (R.mode != READER_HEX) {
        ADD(M_FIND, "Find...");
        if (R.query[0]) ADD(M_FIND_NEXT, "Find next");
    }
    if (R.mode == READER_PDF && R.pages > 1) ADD(M_GOTO, "Go to page...");
    if (R.mode == READER_TEXT) {
        ADD(M_ENC, "Encoding");
        snprintf(it[k - 1].value, sizeof(it[k - 1].value), "%s",
                 R.enc == ENC_AUTO ? (R.enc_used == ENC_UTF8 ? "Auto: UTF-8" : "Auto: Western") : enc_name(R.enc));
        it[k - 1].gap = 1;
    }
    if (R.mode == READER_PDF) {
        ADD(M_LEGACY, "Old Greek fonts");
        snprintf(it[k - 1].value, sizeof(it[k - 1].value), "%s", R.legacy ? "On" : "Off");
        it[k - 1].gap = 1;
    }
    if (R.mode != READER_HEX) {
        ADD(M_SIZE, "Text size");
        snprintf(it[k - 1].value, sizeof(it[k - 1].value), "%s", R.font == FONT_SMALL ? "Small" : "Large");
        if (R.mode == READER_PDF && R.info.img_off && R.len)
            ADD(M_IMAGE, R.show_img ? "Show the text" : "Show the page picture");
    }
    if (R.mode != READER_PDF) {
        ADD(R.mode == READER_HEX ? M_HEX : M_HEX, R.mode == READER_HEX ? "View as text" : "View as hex");
        it[k - 1].gap = R.mode == READER_HEX;
    }
    ADD(M_START, "Go to the start");
    it[k - 1].gap = 1;
    ADD(M_END, "Go to the end");
#undef ADD
    *n = k;
}

static void open_menu(void)
{
    MenuItem items[MENU_MAX];
    int n;
    fill_menu(items, &n);
    char sub[64];
    if (R.mode == READER_PDF) snprintf(sub, sizeof(sub), "PDF" DOT "%d pages", R.pages);
    else if (R.mode == READER_HEX) snprintf(sub, sizeof(sub), "Hex view");
    else snprintf(sub, sizeof(sub), "Text" DOT "%d lines", R.nlines);
    menu_open(R.name, sub, items, n, menu_cb);
}

static void scroll(int d)
{
    if (showing_img()) {
        R.img_top += d * (d == 1 || d == -1 ? 24 : 1);
        R.img_dirty = 1;
        return;
    }
    R.top += d;
    clamp_top();
}

int reader_input(unsigned int p)
{
    if (!R.open)
        return 0;
    int screen = showing_img() ? IMG_VIEW_H - 24 : rows() - 1;
    if (p & PSP_CTRL_CIRCLE) {
        close_reader();
        return 1;
    }
    if (p & PSP_CTRL_TRIANGLE) {
        if (!R.loading || R.err[0])
            open_menu();
        return 1;
    }
    if (R.loading)
        return 1;
    if (p & PSP_CTRL_UP) scroll(-1);
    else if (p & PSP_CTRL_DOWN) scroll(1);
    else if (p & PSP_CTRL_LEFT) scroll(-screen);
    else if (p & PSP_CTRL_RIGHT) scroll(screen);
    else if (p & PSP_CTRL_CROSS) {
        int at_end = showing_img() ? R.img_top + IMG_VIEW_H >= R.ih : R.top + rows() >= total_rows();
        if (at_end && R.mode == READER_PDF && R.page + 1 < R.pages)
            go_page(R.page + 1);
        else
            scroll(screen);
    } else if (p & PSP_CTRL_LTRIGGER) {
        if (R.mode == READER_PDF) go_page(R.page - 1);
        else { R.top = 0; }
    } else if (p & PSP_CTRL_RTRIGGER) {
        if (R.mode == READER_PDF) go_page(R.page + 1);
        else { R.top = total_rows(); clamp_top(); }
    } else if (p & PSP_CTRL_SQUARE && R.mode != READER_HEX) {
        if (R.query[0]) find_next();
        else osk_open("Find", "", OSK_TEXT, on_find);
    }
    return 1;
}

int reader_step(void)
{
    int old = R.t.x;
    spring_step(&R.t);
    int dirty = R.t.x != old;
    if (s_ready) {
        Result r = s_res;
        __sync_synchronize();
        s_ready = 0;
        dirty = 1;
        if (r.gen != R.gen || !R.open) {
            free_later(r.text, r.lines, r.bytes, r.img);
        } else if (r.kind == J_WRAP) {
            free_later(R.lines, NULL, NULL, NULL);
            R.lines = r.lines;
            R.nlines = r.ok ? r.nlines : 0;
            if (R.pending_jump >= 0 && R.nlines) {
                R.top = line_of(R.pending_jump);
                R.pending_jump = -1;
            }
            clamp_top();
        } else if (r.kind == J_PDF_FIND) {
            R.loading = 0;
            if (!r.ok) {
                snprintf(R.err, sizeof(R.err), "%s", r.err);
            } else if (r.found_page < 0) {
                ui_toast("Not found");
                R.hl_len = 0;
            } else if (r.found_page == R.page) {
                R.hl_off = r.found_off;
                R.hl_len = (int)strlen(R.query);
                R.top = line_of(r.found_off) - 2;
                clamp_top();
            } else {
                R.pending_jump = r.found_off;
                R.show_img = 0;
                post_job(J_PDF_PAGE, r.found_page, 0);
            }
        } else {
            R.loading = 0;
            drop_doc_buffers();
            if (!r.ok) {
                snprintf(R.err, sizeof(R.err), "%s", r.err);
                free_later(r.text, r.lines, r.bytes, r.img);
            } else {
                R.err[0] = 0;
                R.mode = r.mode;
                R.text = r.text;
                R.len = r.len;
                R.lines = r.lines;
                R.nlines = r.nlines;
                R.bytes = r.bytes;
                R.nbytes = r.nbytes;
                R.truncated = r.truncated;
                R.file_size = r.file_size;
                R.enc_used = r.enc_used;
                if (r.kind == J_PDF_OPEN || r.kind == J_PDF_PAGE) {
                    if (r.page != R.page || r.kind == J_PDF_OPEN) { /* a new page starts at its top */
                        R.top = 0;
                        R.img_top = 0;
                    }
                    R.pages = r.pages;
                    R.page = r.page;
                    R.info = r.info;
                    R.img = r.img;
                    R.iw = r.iw;
                    R.ih = r.ih;
                    R.img_dirty = 1;
                    if (R.pending_jump >= 0) {
                        R.hl_off = R.pending_jump;
                        R.hl_len = (int)strlen(R.query);
                        R.top = line_of(R.pending_jump) - 2;
                        R.pending_jump = -1;
                    }
                }
                if (R.use_mark && r.mark_off > 0 && R.nlines) /* back where it was left */
                    R.top = line_of(r.mark_off);
                R.use_mark = 0;
                clamp_top();
            }
        }
    }
    if (R.img_dirty && showing_img())
        upload_img();
    return dirty;
}

/* ------------------------------------------------------------------- draw */

static int measure(int font, const char *s, int n)
{
    int w = 0;
    for (const char *e = s + n; s < e;)
        w += gfx_glyph_adv(font, &s);
    return w;
}

static void draw_text(void)
{
    int lh = line_h(), n = rows();
    for (int k = 0; k < n && R.top + k < R.nlines; k++) {
        int i = R.top + k;
        int off = R.lines[i], end = i + 1 < R.nlines ? R.lines[i + 1] : R.len;
        while (end > off && (R.text[end - 1] == '\n' || R.text[end - 1] == ' '))
            end--;
        int y = TOP + k * lh;
        if (R.hl_len && R.hl_off < end && R.hl_off + R.hl_len > off) {
            int a = R.hl_off > off ? R.hl_off : off, b = R.hl_off + R.hl_len < end ? R.hl_off + R.hl_len : end;
            int x0 = TEXT_X + measure(R.font, R.text + off, a - off);
            gfx_rrect(x0 - 1, y, measure(R.font, R.text + a, b - a) + 2, lh, 3, RGB(0x6e5a1f));
        }
        gfx_text_n(R.font, TEXT_X, y, R.text + off, end - off, C_TEXT);
    }
}

static void draw_hex(void)
{
    static const char HEX[] = "0123456789ABCDEF";
    int lh = gfx_line_h(FONT_SMALL), n = rows();
    int cw = gfx_text_width(FONT_SMALL, "0"); /* digits are equally wide */
    int step = cw * 2 + 3; /* 16 bytes, a gap after 8, then the characters: fits in 450 px */
    int ox = TEXT_X, bx = ox + cw * 8 + 10, ax = bx + 16 * step + 4 + 8;
    for (int k = 0; k < n; k++) {
        int off = (R.top + k) * 16;
        if (off >= R.nbytes)
            break;
        int y = TOP + k * lh;
        char b[16];
        snprintf(b, sizeof(b), "%08X", off);
        for (int c = 0; c < 8; c++) {
            char ch[2] = {b[c], 0};
            gfx_text(FONT_SMALL, ox + c * cw, y, ch, C_MUTED);
        }
        for (int i = 0; i < 16 && off + i < R.nbytes; i++) {
            uint8_t v = R.bytes[off + i];
            char h[3] = {HEX[v >> 4], HEX[v & 15], 0};
            int x = bx + i * step + (i >= 8 ? 4 : 0);
            char c1[2] = {h[0], 0}, c2[2] = {h[1], 0};
            gfx_text_center(FONT_SMALL, x + cw / 2, y, c1, v ? C_TEXT : C_MUTED);
            gfx_text_center(FONT_SMALL, x + cw + cw / 2, y, c2, v ? C_TEXT : C_MUTED);
            char a[2] = {v >= 32 && v < 127 ? (char)v : '.', 0};
            gfx_text_center(FONT_SMALL, ax + i * 6 + 3, y, a, v >= 32 && v < 127 ? C_TEXT : C_LINE);
        }
    }
}

static void draw_scrollbar(int pos, int span, int total)
{
    if (total <= span || total <= 0)
        return;
    int h = BOTTOM - TOP, bh = h * span / total;
    if (bh < 12) bh = 12;
    int y = TOP + (h - bh) * pos / (total - span);
    gfx_rect(SCREEN_W - 6, TOP, 2, h, C_LINE);
    gfx_rrect(SCREEN_W - 7, y, 4, bh, 2, C_MUTED);
}

void reader_draw(const SysStatus *st)
{
    int t = R.t.x;
    if (t <= 0)
        return;
    if (t > 256) t = 256;
    gfx_set_opacity(t);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, C_BG);

    char title[160], pos[48] = "";
    if (R.mode == READER_PDF && R.pages)
        snprintf(pos, sizeof(pos), "Page %d / %d", R.page + 1, R.pages);
    else if (R.mode == READER_TEXT && R.nlines > rows())
        snprintf(pos, sizeof(pos), "%d%%", (R.top + rows()) * 100 / R.nlines > 100 ? 100 : (R.top + rows()) * 100 / R.nlines);
    else if (R.mode == READER_HEX && R.nbytes)
        snprintf(pos, sizeof(pos), "0x%X", R.top * 16);
    /* the name, cut to fit before the clock */
    int maxw = 236 - (pos[0] ? gfx_text_width(FONT_SMALL, pos) + 14 : 0);
    snprintf(title, sizeof(title), "%s", R.name);
    if (gfx_text_width(FONT_SMALL, title) > maxw) {
        int n = (int)strlen(title);
        while (n > 0 && gfx_text_width(FONT_SMALL, title) + gfx_text_width(FONT_SMALL, "...") > maxw) {
            do n--; while (n > 0 && ((unsigned char)title[n] & 0xC0) == 0x80);
            title[n] = 0;
        }
        strcat(title, "...");
    }
    if (pos[0]) {
        strncat(title, DOT, sizeof(title) - strlen(title) - 1);
        strncat(title, pos, sizeof(title) - strlen(title) - 1);
    }
    ui_draw_status_bar(title, st);

    if (R.err[0]) {
        gfx_icon(R.mode == READER_PDF ? ICON_PDF : ICON_DOC, SCREEN_W / 2 - 16, 92, 32, 32, C_MUTED);
        gfx_text_center(FONT_BODY, SCREEN_W / 2, 136, R.err, C_MUTED);
    } else if (R.loading && !R.text && !R.bytes) {
        gfx_text_center(FONT_BODY, SCREEN_W / 2, 126, R.mode == READER_PDF ? "Opening the PDF..." : "Loading...", C_MUTED);
    } else if (showing_img()) {
        const void *tex = (const void *)((u32)sceGeEdramGetAddr() + GFX_VRAM_VIEWER);
        int h = R.ih < IMG_VIEW_H ? R.ih : IMG_VIEW_H;
        gfx_texture(tex, GU_PSM_5650, 512, 512, TBW, 0, R.iw, h, (SCREEN_W - R.iw) / 2,
                    TOP - 2 + (R.ih < IMG_VIEW_H ? (IMG_VIEW_H - R.ih) / 2 : 0), R.iw, h);
        draw_scrollbar(R.img_top, IMG_VIEW_H, R.ih);
    } else if (R.mode == READER_HEX) {
        draw_hex();
        draw_scrollbar(R.top, rows(), total_rows());
    } else if (R.mode == READER_PDF && R.len == 0) {
        const char *msg = R.info.undecodable ? "This page's font has no text map, so its text can't be shown"
                        : "No text on this page";
        gfx_icon(ICON_PDF, SCREEN_W / 2 - 16, 92, 32, 32, C_MUTED);
        gfx_text_center(FONT_SMALL, SCREEN_W / 2, 136, msg, C_MUTED);
    } else if (R.text) {
        draw_text();
        draw_scrollbar(R.top, rows(), R.nlines);
    }
    if (R.loading && (R.text || R.bytes)) {
        int w = gfx_text_width(FONT_SMALL, "Loading...") + 20;
        gfx_rrect(SCREEN_W - w - 12, TOP + 2, w, 20, 10, 0xC0000000u);
        gfx_text(FONT_SMALL, SCREEN_W - w - 2, TOP + 5, "Loading...", C_TEXT);
    }
    if (R.truncated && !R.err[0] && R.mode != READER_PDF && R.top + rows() >= total_rows()) {
        char note[64];
        snprintf(note, sizeof(note), "Showing the first %d MB", (R.mode == READER_HEX ? HEX_MAX : TEXT_MAX) >> 20);
        gfx_text_right(FONT_SMALL, SCREEN_W - 14, BOTTOM - 16, note, C_FOCUS);
    }

    gfx_rect(0, FOOTER_Y, SCREEN_W, 1, C_LINE);
    static const Hint pdf_hints[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Next"},
        {-1, "L/R", 0, "Page"},
        {ICON_BTN_SQUARE, NULL, C_BTN_SQUARE, "Find"},
        {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Menu"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
    };
    static const Hint text_hints[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Next"},
        {-1, "L/R", 0, "Start / end"},
        {ICON_BTN_SQUARE, NULL, C_BTN_SQUARE, "Find"},
        {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Menu"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
    };
    static const Hint hex_hints[] = {
        {-1, "<-/->", 0, "Page"},
        {-1, "L/R", 0, "Start / end"},
        {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Menu"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
    };
    if (R.mode == READER_PDF)
        ui_draw_hints(pdf_hints, COUNT(pdf_hints), SCREEN_W / 2, FOOTER_Y + 5);
    else if (R.mode == READER_HEX)
        ui_draw_hints(hex_hints, COUNT(hex_hints), SCREEN_W / 2, FOOTER_Y + 5);
    else
        ui_draw_hints(text_hints, COUNT(text_hints), SCREEN_W / 2, FOOTER_Y + 5);
    gfx_set_opacity(256);
}
