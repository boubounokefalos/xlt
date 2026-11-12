/* The ZIP reader against archives from Python's zipfile (tests/make_zips.py),
 * some damaged on purpose, and the path rules that keep an archive inside its folder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zip.h"

static int fails;
static const char *s_dir;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int file_read(void *ctx, uint64_t off, void *dst, uint32_t len)
{
    FILE *f = ctx;
    if (fseek(f, (long)off, SEEK_SET))
        return -1;
    return (int)fread(dst, 1, len, f);
}

typedef struct { unsigned char *b; size_t n, cap; int fail_after; } Sink;

static int sink_write(void *ctx, const void *src, uint32_t len)
{
    Sink *s = ctx;
    if (s->fail_after >= 0 && s->n + len > (size_t)s->fail_after)
        return -1;
    if (s->n + len > s->cap) {
        s->cap = (s->n + len) * 2;
        s->b = realloc(s->b, s->cap);
    }
    memcpy(s->b + s->n, src, len);
    s->n += len;
    return (int)len;
}

static FILE *open_zip(const char *name, ZipDir *z, int *r)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", s_dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) {
        printf("missing %s\n", p);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    *r = zip_open(z, file_read, f, (uint64_t)size);
    return f;
}

static uint8_t s_in[4096], s_out[8192];

/* Extracts the entry with this name. */
static int get(const char *zipname, const char *entry, Sink *s, volatile int *cancel)
{
    ZipDir z;
    ZipEntry e;
    int r, found = ZIP_ERR_READ;
    FILE *f = open_zip(zipname, &z, &r);
    memset(s, 0, sizeof(*s));
    s->fail_after = -1;
    if (r == ZIP_OK) {
        while (zip_next(&z, &e) > 0)
            if (!strcmp(e.name, entry))
                found = zip_extract(&z, &e, sink_write, s, s_in, sizeof(s_in), s_out, sizeof(s_out), cancel);
        zip_close(&z);
    }
    fclose(f);
    return r == ZIP_OK ? found : r;
}

static int open_result(const char *name)
{
    ZipDir z;
    int r;
    FILE *f = open_zip(name, &z, &r);
    if (r == ZIP_OK) zip_close(&z);
    fclose(f);
    return r;
}

static void big_ok(const Sink *s, const char *what)
{
    CHECK(s->n == 300000, "%s: %zu bytes", what, s->n);
    int same = 1;
    for (int i = 0; i < 300000 && (size_t)i < s->n; i++)
        if (s->b[i] != (unsigned char)((i * 7 + (i >> 8)) & 255)) same = 0;
    CHECK(same, "%s: contents differ", what);
}

static void plan(const char *name, int kind, const char *app)
{
    ZipDir z;
    ZipPlan p;
    int r;
    FILE *f = open_zip(name, &z, &r);
    CHECK(r == ZIP_OK, "%s opens: %d", name, r);
    zip_install_plan(&z, &p);
    CHECK(p.kind == kind, "%s: plan %d, wanted %d", name, p.kind, kind);
    CHECK(!strcmp(p.name, app), "%s: name '%s'", name, p.name);
    ZipEntry e; /* the plan leaves the list at its start */
    CHECK(zip_next(&z, &e) == 1, "%s: list rewound", name);
    zip_close(&z);
    fclose(f);
}

static void safe(const char *raw, const char *want)
{
    char out[ZIP_PATH];
    int r = zip_safe_path(raw, out, sizeof(out));
    if (want) CHECK(r == 0 && !strcmp(out, want), "'%s' -> '%s' (%d), wanted '%s'", raw, out, r, want);
    else CHECK(r != 0, "'%s' should be refused, got '%s'", raw, out);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    s_dir = argv[1];
    Sink s;

    /* listing */
    ZipDir z;
    ZipEntry e;
    int r;
    FILE *f = open_zip("basic.zip", &z, &r);
    CHECK(r == ZIP_OK, "basic opens: %d", r);
    CHECK(z.count == 4, "4 entries: %u", z.count);
    int n = 0, dirs = 0;
    while ((r = zip_next(&z, &e)) > 0) { n++; dirs += e.is_dir; }
    CHECK(r == 0 && n == 4 && dirs == 1, "listed %d, %d dirs, end %d", n, dirs, r);
    zip_rewind(&z);
    CHECK(zip_next(&z, &e) == 1 && !strcmp(e.name, "hello.txt"), "rewind: %s", e.name);
    zip_close(&z);
    fclose(f);

    /* contents */
    CHECK(get("basic.zip", "hello.txt", &s, NULL) == ZIP_OK && s.n == 12 && !memcmp(s.b, "hello world\n", 12), "deflated small");
    free(s.b);
    CHECK(get("basic.zip", "dir/big.bin", &s, NULL) == ZIP_OK, "deflated big");
    big_ok(&s, "deflated");
    free(s.b);
    CHECK(get("basic.zip", "empty.txt", &s, NULL) == ZIP_OK && s.n == 0, "empty file");
    free(s.b);
    CHECK(get("stored.zip", "big.bin", &s, NULL) == ZIP_OK, "stored big");
    big_ok(&s, "stored");
    free(s.b);
    CHECK(get("descriptor.zip", "streamed.bin", &s, NULL) == ZIP_OK, "data descriptor");
    big_ok(&s, "descriptor");
    free(s.b);
    CHECK(get("comment.zip", "a.txt", &s, NULL) == ZIP_OK && s.n == 1, "archive comment");
    free(s.b);
    CHECK(get("stub.zip", "a.txt", &s, NULL) == ZIP_OK && s.n == 12, "stub before the archive");
    free(s.b);

    /* refused or damaged */
    CHECK(open_result("notzip.zip") == ZIP_ERR_NOT_ZIP, "not a zip");
    CHECK(open_result("tiny.zip") == ZIP_ERR_NOT_ZIP, "two bytes");
    CHECK(open_result("truncated.zip") == ZIP_ERR_NOT_ZIP, "truncated");
    CHECK(open_result("zip64.zip") == ZIP_ERR_ZIP64, "zip64");
    CHECK(get("badcrc.zip", "a.txt", &s, NULL) == ZIP_ERR_DATA, "bad checksum");
    free(s.b);
    CHECK(get("baddeflate.zip", "big.bin", &s, NULL) == ZIP_ERR_DATA, "bad deflate data");
    free(s.b);
    CHECK(get("liar.zip", "big.bin", &s, NULL) == ZIP_ERR_DATA, "more than it declared");
    CHECK(s.n <= 1000, "stopped at the declared size: %zu", s.n);
    free(s.b);
    CHECK(get("locked.zip", "a.txt", &s, NULL) == ZIP_ERR_ENCRYPTED, "password");
    free(s.b);
    CHECK(get("method.zip", "a.txt", &s, NULL) == ZIP_ERR_METHOD, "unknown method");
    free(s.b);
    f = open_zip("badlist.zip", &z, &r);
    CHECK(r == ZIP_OK && zip_next(&z, &e) == ZIP_ERR_DATA, "list running off its end");
    zip_close(&z);
    fclose(f);

    /* cancel, and a full stick */
    volatile int stop = 1;
    CHECK(get("basic.zip", "dir/big.bin", &s, &stop) == ZIP_CANCELLED, "cancel");
    free(s.b);
    f = open_zip("basic.zip", &z, &r);
    while (zip_next(&z, &e) > 0)
        if (!strcmp(e.name, "dir/big.bin")) {
            memset(&s, 0, sizeof(s));
            s.fail_after = 1000;
            CHECK(zip_extract(&z, &e, sink_write, &s, s_in, sizeof(s_in), s_out, sizeof(s_out), NULL) == ZIP_ERR_WRITE, "write fails");
            free(s.b);
        }
    zip_close(&z);
    fclose(f);

    /* paths */
    safe("a.txt", "a.txt");
    safe("dir/sub/a.txt", "dir/sub/a.txt");
    safe("dir/", "dir");
    safe("dir\\sub\\a.txt", "dir/sub/a.txt");
    safe("./a.txt", "a.txt");
    safe("a//b.txt", "a/b.txt");
    safe("a/./b.txt", "a/b.txt");
    safe("../evil.txt", NULL);
    safe("a/../../evil.txt", NULL);
    safe("..\\evil.txt", NULL);
    safe("/etc/passwd", NULL);
    safe("\\evil.txt", NULL);
    safe("ms0:/PSP/evil", NULL);
    safe("C:\\evil.txt", NULL);
    safe("", NULL);
    safe("a\nb", NULL);
    safe(".", NULL);
    safe("a*b?.txt", "a_b_.txt");
    safe("b:c.txt", NULL);        /* a drive to anything that splits at ':' */
    safe("dir/b:c.txt", "dir/b_c.txt");
    safe("name ./x", "name/x");   /* FAT drops the trailing dot and space */
    safe("caf\xC3\xA9.txt", "caf\xC3\xA9.txt");
    safe("\xCE\xB1\xCE\xB2.txt", "\xCE\xB1\xCE\xB2.txt");
    safe("caf\x82.txt", "caf_.txt"); /* CP437, not UTF-8 */
    safe("//", NULL);
    char longname[400];
    memset(longname, 'a', sizeof(longname));
    longname[399] = 0;
    safe(longname, NULL);
    CHECK(zip_is_junk("__MACOSX/a/b"), "mac folder");
    CHECK(zip_is_junk("x/.DS_Store"), "ds_store");
    CHECK(zip_is_junk("Thumbs.db"), "thumbs");
    CHECK(!zip_is_junk("__MACOSX2/a"), "similar folder");
    CHECK(!zip_is_junk("readme.txt"), "ordinary file");
    for (int i = -10; i < 0; i++)
        CHECK(zip_error_text(i) && zip_error_text(i)[0], "error text %d", i);

    /* where homebrew goes */
    plan("plan_game.zip", ZIP_INSTALL_PSP_GAME, "MyApp");
    plan("plan_folder.zip", ZIP_INSTALL_FOLDER, "MyApp");
    plan("plan_bare.zip", ZIP_INSTALL_BARE, "");
    plan("plan_lower.zip", ZIP_INSTALL_PSP_GAME, "MyApp");
    plan("plan_two_tops.zip", ZIP_INSTALL_NONE, "");
    plan("plan_none.zip", ZIP_INSTALL_NONE, "");
    plan("plan_deep.zip", ZIP_INSTALL_NONE, "");
    plan("plan_mixed.zip", ZIP_INSTALL_NONE, "");
    plan("plan_plugins.zip", ZIP_INSTALL_NONE, "");
    plan("plan_evil.zip", ZIP_INSTALL_NONE, "");

    if (fails) {
        printf("test_zip: %d failed\n", fails);
        return 1;
    }
    puts("test_zip: ok");
    return 0;
}
