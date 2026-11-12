/* XLT - copy / move / delete / size on a worker thread. See fileops.h.
 *
 * The thread runs below the UI (priority 0x30), so browsing stays smooth
 * while it works. A job first counts what it will touch (for the progress
 * bar), then does it in 256 KB chunks, checking for cancel between chunks.
 * A file that couldn't be copied completely is removed again. Moves on the
 * same device are a rename; across devices (ms0: <-> ef0:) a copy and then
 * a delete of what was copied completely.
 */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "fileops.h"
#include "zip.h"

#ifdef __PSP__
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <malloc.h>
#include <stdlib.h>

#define CHUNK (256 * 1024)
#define MAX_DEPTH 12
#define CST_CT 0x0008
#define CST_MT 0x0020

static SceUID s_thread = -1, s_sema = -1;
static volatile int s_busy, s_cancel, s_result_ready;
static FopStatus s_st;
static int s_op, s_n;
static char s_paths[FOP_MAX_ITEMS][FOP_PATH];
static char s_dest[FOP_PATH];
static unsigned char *s_buf;

static int exists(const char *p)
{
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    return sceIoGetstat(p, &st) >= 0;
}

static void set_current(const char *path)
{
    snprintf(s_st.current, sizeof(s_st.current), "%s", fop_basename(path));
}

static void error(const char *what, const char *path)
{
    if (!s_st.errors)
        snprintf(s_st.first_error, sizeof(s_st.first_error), "%s %s", what, fop_basename(path));
    s_st.errors++;
}

static int join(char *out, const char *dir, const char *name)
{
    int n = (int)strlen(dir);
    return snprintf(out, FOP_PATH, n && dir[n - 1] == '/' ? "%s%s" : "%s/%s", dir, name) < FOP_PATH;
}

/* ------------------------------------------------------------- counting */

static void count_tree(const char *path, int depth, int with_dirs)
{
    if (s_cancel || depth > MAX_DEPTH)
        return;
    SceUID d = sceIoDopen(path);
    if (d < 0)
        return;
    SceIoDirent de;
    memset(&de, 0, sizeof(de));
    while (!s_cancel && sceIoDread(d, &de) > 0) {
        if (strcmp(de.d_name, ".") && strcmp(de.d_name, "..")) {
            char p[FOP_PATH];
            if (join(p, path, de.d_name)) {
                if (FIO_S_ISDIR(de.d_stat.st_mode)) {
                    if (with_dirs) s_st.items_total++;
                    count_tree(p, depth + 1, with_dirs);
                } else {
                    s_st.items_total++;
                    s_st.bytes_total += (uint64_t)de.d_stat.st_size;
                }
            }
        }
        memset(&de, 0, sizeof(de));
    }
    sceIoDclose(d);
}

static void count_item(const char *path, int with_dirs)
{
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    if (sceIoGetstat(path, &st) < 0)
        return;
    if (FIO_S_ISDIR(st.st_mode)) {
        if (with_dirs) s_st.items_total++;
        count_tree(path, 0, with_dirs);
    } else {
        s_st.items_total++;
        s_st.bytes_total += (uint64_t)st.st_size;
    }
}

/* ---------------------------------------------------------------- copy */

/* 0 copied, -1 failed (and removed), -2 cancelled */
static int copy_file(const char *src, const char *dst)
{
    set_current(src);
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    sceIoGetstat(src, &st);
    SceUID in = sceIoOpen(src, PSP_O_RDONLY, 0);
    if (in < 0) {
        error("Couldn't read", src);
        return -1;
    }
    SceUID out = sceIoOpen(dst, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (out < 0) {
        sceIoClose(in);
        error("Couldn't create", dst);
        return -1;
    }
    int r = 0;
    for (;;) {
        if (s_cancel) { r = -2; break; }
        int n = sceIoRead(in, s_buf, CHUNK);
        if (n < 0) { error("Couldn't read", src); r = -1; break; }
        if (n == 0) break;
        int w = sceIoWrite(out, s_buf, n);
        if (w != n) {
            s_st.full = 1; /* almost always: no room left */
            error("No room for", src);
            r = -1;
            break;
        }
        s_st.bytes_done += (uint64_t)n;
    }
    sceIoClose(in);
    sceIoClose(out);
    if (r < 0) {
        sceIoRemove(dst);
        return r;
    }
    sceIoChstat(dst, &st, CST_CT | CST_MT); /* keep the dates (Pictures sorts by them) */
    s_st.items_done++;
    return 0;
}

static int copy_tree(const char *src, const char *dst, int depth)
{
    if (depth > MAX_DEPTH) {
        error("Too deep:", src);
        return -1;
    }
    if (sceIoMkdir(dst, 0777) < 0 && !exists(dst)) {
        error("Couldn't create", dst);
        return -1;
    }
    SceUID d = sceIoDopen(src);
    if (d < 0) {
        error("Couldn't open", src);
        return -1;
    }
    int r = 0;
    SceIoDirent de;
    memset(&de, 0, sizeof(de));
    while (sceIoDread(d, &de) > 0) {
        if (s_cancel) { r = -2; break; }
        if (strcmp(de.d_name, ".") && strcmp(de.d_name, "..")) {
            char a[FOP_PATH], b[FOP_PATH];
            if (!join(a, src, de.d_name) || !join(b, dst, de.d_name)) {
                error("Name too long:", de.d_name);
                r = -1;
            } else {
                int rr = FIO_S_ISDIR(de.d_stat.st_mode) ? copy_tree(a, b, depth + 1) : copy_file(a, b);
                if (rr == -2 || s_st.full) { r = rr == -2 ? -2 : -1; break; }
                if (rr < 0) r = -1;
            }
        }
        memset(&de, 0, sizeof(de));
    }
    sceIoDclose(d);
    return r;
}

/* -------------------------------------------------------------- delete */

static int remove_tree(const char *dir, int depth)
{
    if (depth > MAX_DEPTH)
        return -1;
    int failed = 0;
    for (;;) { /* reopen after every removal: entries vanish under an open handle */
        if (s_cancel)
            return -2;
        SceUID d = sceIoDopen(dir);
        if (d < 0)
            return -1;
        SceIoDirent de;
        char path[FOP_PATH];
        int got = 0, fits = 0, is_dir = 0, seen = 0;
        memset(&de, 0, sizeof(de));
        while (sceIoDread(d, &de) > 0) {
            if (strcmp(de.d_name, ".") && strcmp(de.d_name, "..") && seen++ >= failed) {
                got = 1;
                fits = join(path, dir, de.d_name);
                is_dir = FIO_S_ISDIR(de.d_stat.st_mode);
                break;
            }
            memset(&de, 0, sizeof(de));
        }
        sceIoDclose(d);
        if (!got)
            break;
        if (!fits) {
            failed++;
            continue;
        }
        set_current(path);
        int r = is_dir ? remove_tree(path, depth + 1) : sceIoRemove(path);
        if (r == -2)
            return -2;
        if (r < 0) {
            error("Couldn't delete", path);
            failed++;
        } else {
            s_st.items_done++;
        }
    }
    if (sceIoRmdir(dir) < 0)
        return -1;
    return failed ? -1 : 0;
}

static int delete_item(const char *path)
{
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    if (sceIoGetstat(path, &st) < 0)
        return 0;
    set_current(path);
    int r;
    if (FIO_S_ISDIR(st.st_mode))
        r = remove_tree(path, 0);
    else
        r = sceIoRemove(path) < 0 ? -1 : 0;
    if (r == -2)
        return -2;
    if (r < 0)
        error("Couldn't delete", path);
    else
        s_st.items_done++;
    return r;
}

/* ------------------------------------------------------------- extract */

static int zip_read(void *ctx, uint64_t off, void *dst, uint32_t len)
{
    SceUID fd = *(SceUID *)ctx;
    if (sceIoLseek(fd, (SceOff)off, PSP_SEEK_SET) < 0)
        return -1;
    return sceIoRead(fd, dst, len);
}

static int zip_write(void *ctx, const void *src, uint32_t len)
{
    if (sceIoWrite(*(SceUID *)ctx, src, len) != (int)len)
        return -1;
    s_st.bytes_done += len;
    return (int)len;
}

/* Makes every folder of a path that doesn't exist yet. */
static void make_dirs(const char *path)
{
    static char last[FOP_PATH]; /* archives list many files in a row from one folder */
    if (!strcmp(last, path) && exists(path))
        return;
    char p[FOP_PATH];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = strchr(p, '/'); s; s = strchr(s + 1, '/')) {
        if (s[1] == 0)
            break;
        *s = 0;
        if (s[-1] != ':') /* "ms0:" is no folder */
            sceIoMkdir(p, 0777);
        *s = '/';
    }
    sceIoMkdir(p, 0777);
    snprintf(last, sizeof(last), "%s", path);
}

/* One file (or folder) out of the archive. -1 to stop the whole job. */
static int extract_entry(const ZipDir *z, const ZipEntry *e, const char *safe, const char *dest, int over)
{
    char dst[FOP_PATH], parent[FOP_PATH];
    if (!join(dst, dest, safe)) {
        error("Name too long:", safe);
        return 0;
    }
    if (e->is_dir) {
        make_dirs(dst);
        s_st.items_done++;
        return 0;
    }
    if (e->encrypted || (e->method != 0 && e->method != 8)) {
        error(zip_error_text(e->encrypted ? ZIP_ERR_ENCRYPTED : ZIP_ERR_METHOD), safe);
        return 0;
    }
    const char *leaf = fop_basename(dst);
    snprintf(parent, sizeof(parent), "%.*s", (int)(leaf - dst) - 1, dst);
    make_dirs(parent);
    if (!over && exists(dst)) { /* never over what is already there */
        char uname[FOP_PATH];
        fop_unique_name(parent, leaf, uname, sizeof(uname), exists);
        if (!join(dst, parent, uname)) {
            error("Name too long:", safe);
            return 0;
        }
    }
    set_current(safe);
    SceUID out = sceIoOpen(dst, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (out < 0) {
        error("Couldn't create", safe);
        return 0;
    }
    int half = CHUNK / 2;
    int r = zip_extract(z, e, zip_write, &out, s_buf, half, s_buf + half, half, &s_cancel);
    sceIoClose(out);
    if (r == ZIP_OK) {
        s_st.items_done++;
        return 0;
    }
    sceIoRemove(dst); /* a file half written is worth nothing */
    if (r == ZIP_CANCELLED)
        return -1;
    error(zip_error_text(r), safe);
    if (r == ZIP_ERR_WRITE) {
        s_st.full = 1;
        return -1;
    }
    return 0;
}

static void extract_zip(const char *zip, const char *dest, int over)
{
    set_current(zip);
    SceUID in = sceIoOpen(zip, PSP_O_RDONLY, 0);
    if (in < 0) {
        error("Couldn't open", zip);
        return;
    }
    ZipDir z;
    int r = zip_open(&z, zip_read, &in, (uint64_t)sceIoLseek(in, 0, PSP_SEEK_END));
    if (r != ZIP_OK) {
        error(zip_error_text(r), zip);
        sceIoClose(in);
        return;
    }
    ZipEntry e;
    char safe[ZIP_PATH];
    while (zip_next(&z, &e) > 0) /* what the bar counts */
        if (zip_safe_path(e.name, safe, sizeof(safe)) == 0 && !zip_is_junk(safe)) {
            s_st.items_total++;
            if (!e.is_dir) s_st.bytes_total += e.usize;
        }
    s_st.phase = FOP_WORKING;
    make_dirs(dest);
    zip_rewind(&z);
    while (!s_cancel && (r = zip_next(&z, &e)) > 0) {
        if (zip_safe_path(e.name, safe, sizeof(safe)) != 0) {
            error("Skipped an unsafe path in", zip); /* it would land outside the folder */
            continue;
        }
        if (!zip_is_junk(safe) && extract_entry(&z, &e, safe, dest, over) < 0)
            break;
    }
    if (r < 0)
        error(zip_error_text(r), zip);
    zip_close(&z);
    sceIoClose(in);
}

/* ----------------------------------------------------------------- job */

static int same_device(const char *a, const char *b) { return !strncmp(a, b, 4); }

static void run_job(void)
{
    if (s_op == FOP_EXTRACT || s_op == FOP_EXTRACT_OVER) {
        s_st.phase = FOP_COUNTING;
        extract_zip(s_paths[0], s_dest, s_op == FOP_EXTRACT_OVER);
        return;
    }
    int with_dirs = s_op == FOP_DELETE;
    s_st.phase = FOP_COUNTING;
    for (int i = 0; i < s_n && !s_cancel; i++) {
        if (s_op == FOP_MOVE && same_device(s_paths[i], s_dest)) {
            s_st.items_total++; /* a rename: one step, no bytes */
            continue;
        }
        count_item(s_paths[i], with_dirs);
    }
    if (s_op == FOP_SIZE || s_cancel)
        return;
    s_st.phase = FOP_WORKING;
    for (int i = 0; i < s_n && !s_cancel && !s_st.full; i++) {
        const char *src = s_paths[i];
        if (s_op == FOP_DELETE) {
            if (delete_item(src) == -2)
                break;
            continue;
        }
        SceIoStat st;
        memset(&st, 0, sizeof(st));
        if (sceIoGetstat(src, &st) < 0) {
            error("Not found:", src);
            continue;
        }
        int is_dir = FIO_S_ISDIR(st.st_mode);
        const char *name = fop_basename(src);
        char dst[FOP_PATH], uname[FOP_PATH];
        if (is_dir && fop_is_inside(s_dest, src)) {
            error("Can't put a folder into itself:", src);
            continue;
        }
        /* the source folder itself: moving there does nothing, copying makes "name (2)" */
        char parent[FOP_PATH];
        snprintf(parent, sizeof(parent), "%.*s", (int)(name - src), src);
        int pl = (int)strlen(parent), dl = (int)strlen(s_dest);
        if (pl && parent[pl - 1] == '/') parent[--pl] = 0;
        int same_dir = pl == dl - (dl && s_dest[dl - 1] == '/') && !strncmp(parent, s_dest, pl);
        if (s_op == FOP_MOVE && same_dir) {
            s_st.items_done++;
            continue;
        }
        fop_unique_name(s_dest, name, uname, sizeof(uname), exists);
        if (!join(dst, s_dest, uname)) {
            error("Name too long:", src);
            continue;
        }
        if (s_n == 1 && strlen(uname) < sizeof(s_st.dest_name))
            memcpy(s_st.dest_name, uname, strlen(uname) + 1);
        set_current(src);
        if (s_op == FOP_MOVE && same_device(src, s_dest)) {
            if (sceIoRename(src, dst) >= 0) {
                s_st.items_done++;
                continue;
            }
            /* this firmware won't rename across folders: copy, then delete */
            count_item(src, 0);
            s_st.items_total--;
        }
        int r = is_dir ? copy_tree(src, dst, 0) : copy_file(src, dst);
        if (r == -2)
            break;
        if (s_op == FOP_MOVE && r == 0) {
            int before = s_st.items_done;
            if ((is_dir ? remove_tree(src, 0) : sceIoRemove(src)) < 0 && !s_cancel)
                error("Copied, but couldn't remove", src);
            s_st.items_done = before; /* the removal isn't progress of its own */
        }
    }
}

static int fop_thread(SceSize args, void *argp)
{
    for (;;) {
        if (sceKernelWaitSema(s_sema, 1, NULL) < 0)
            return 0;
        run_job();
        s_st.cancelled = s_cancel;
        s_st.phase = FOP_DONE;
        s_result_ready = 1;
        s_busy = 0;
    }
    return 0;
}

void fop_init(void)
{
    if (s_thread >= 0)
        return;
    s_sema = sceKernelCreateSema("xl_fop", 0, 0, 1, NULL);
    s_thread = sceKernelCreateThread("xl_fop", fop_thread, 0x30, 0x8000, PSP_THREAD_ATTR_USER, NULL);
    if (s_thread >= 0)
        sceKernelStartThread(s_thread, 0, NULL);
}

int fop_start(int op, const char (*paths)[FOP_PATH], int n, const char *dest_dir, const char **why)
{
    static const char *dummy;
    if (!why) why = &dummy;
    *why = NULL;
    if (s_busy || s_thread < 0) {
        *why = "Wait for the current job to finish";
        return -1;
    }
    if (n <= 0)
        return -1;
    if (!s_buf && op != FOP_SIZE && op != FOP_DELETE) {
        s_buf = memalign(64, CHUNK);
        if (!s_buf) {
            *why = "Not enough memory";
            return -1;
        }
    }
    if (n > FOP_MAX_ITEMS) n = FOP_MAX_ITEMS;
    memcpy(s_paths, paths, (size_t)n * FOP_PATH);
    s_n = n;
    s_op = op;
    snprintf(s_dest, sizeof(s_dest), "%s", dest_dir ? dest_dir : "");
    memset(&s_st, 0, sizeof(s_st));
    s_st.op = op;
    s_cancel = 0;
    s_result_ready = 0;
    s_busy = 1;
    sceKernelSignalSema(s_sema, 1);
    return 0;
}

int fop_busy(void) { return s_busy; }
void fop_status(FopStatus *out) { *out = s_st; }
void fop_cancel(void) { if (s_busy) s_cancel = 1; }

int fop_take_result(FopStatus *out)
{
    if (!s_result_ready)
        return 0;
    s_result_ready = 0;
    *out = s_st;
    return 1;
}
#endif /* __PSP__ */

/* ------------------------------------------------------- portable parts */

const char *fop_basename(const char *path)
{
    int n = (int)strlen(path);
    while (n > 0 && path[n - 1] == '/') /* "ms0:/PSP/" -> "PSP" would need a copy; keep the slash */
        n--;
    const char *s = path + n;
    while (s > path && s[-1] != '/' && s[-1] != ':')
        s--;
    return s;
}

void fop_unique_name(const char *dir, const char *name, char *out, int outn, int (*exists)(const char *path))
{
    char p[FOP_PATH];
    int dl = (int)strlen(dir);
    const char *sep = dl && dir[dl - 1] == '/' ? "" : "/";
    snprintf(out, outn, "%s", name);
    snprintf(p, sizeof(p), "%s%s%s", dir, sep, out);
    if (!exists(p))
        return;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name)
        dot = name + strlen(name);
    int stem = (int)(dot - name);
    for (int k = 2; k < 1000; k++) {
        snprintf(out, outn, "%.*s (%d)%s", stem, name, k, dot);
        snprintf(p, sizeof(p), "%s%s%s", dir, sep, out);
        if (!exists(p))
            return;
    }
}

int fop_is_inside(const char *inner, const char *outer)
{
    int n = (int)strlen(outer);
    while (n > 0 && outer[n - 1] == '/')
        n--;
    if (strncasecmp(inner, outer, n))
        return 0;
    return inner[n] == 0 || inner[n] == '/';
}
