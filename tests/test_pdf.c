/* Host test for src/pdf.c and src/textenc.c, on the fixtures from make_pdfs.py.
 * With "dump FILE..." it prints every page instead (for trying real PDFs). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/pdf.h"
#include "../src/textenc.h"

static int fails;
static char buf[1 << 20];

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static PdfDoc *open_fx(const char *dir, const char *name, int *err)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    *err = 0;
    return pdf_open(p, err);
}

static void expect_page(PdfDoc *d, int page, int legacy, const char *want, const char *what)
{
    PdfPageInfo info;
    int n = pdf_page_text(d, page, legacy, buf, sizeof(buf), &info);
    CHECK(n >= 0 && !strcmp(buf, want), "%s page %d: got \"%s\" (%d), want \"%s\"", what, page, n >= 0 ? buf : "", n, want);
}

static void test_textenc(void)
{
    CHECK(enc_cp(ENC_1253, 0xC1) == 0x391, "1253 Alpha");
    CHECK(enc_cp(ENC_1253, 0xE1) == 0x3B1, "1253 alpha");
    CHECK(enc_cp(ENC_1253, 0xDC) == 0x3AC, "1253 alpha tonos");
    CHECK(enc_cp(ENC_1253, 0xF2) == 0x3C2, "1253 final sigma");
    CHECK(enc_cp(ENC_1253, 0xA2) == 0x386, "1253 Alpha tonos");
    CHECK(enc_cp(ENC_1252, 0xE9) == 0xE9, "1252 e acute");
    CHECK(enc_cp(ENC_1252, 0x80) == 0x20AC, "1252 euro");
    char o[8];
    int n = utf8_put(0x3A9, o);
    CHECK(n == 2 && (unsigned char)o[0] == 0xCE && (unsigned char)o[1] == 0xA9, "utf8 omega");
    const uint8_t ok[] = "\xce\x93\xce\xb5\xce\xb9", bad[] = "\xc1\xe9\xe1", cut[] = "ab\xce";
    CHECK(utf8_valid(ok, 6), "valid greek");
    CHECK(!utf8_valid(bad, 3), "1253 bytes are not UTF-8");
    CHECK(utf8_valid(cut, 3), "a character cut by the end");
    int len, used;
    char *t = text_to_utf8(bad, 3, ENC_AUTO, &len, &used);
    CHECK(t && used == ENC_1252 && !strcmp(t, "\xc3\x81\xc3\xa9\xc3\xa1"), "auto falls back to 1252");
    free(t);
    t = text_to_utf8(bad, 3, ENC_1253, &len, &used);
    CHECK(t && !strcmp(t, "\xce\x91\xce\xb9\xce\xb1"), "1253 -> UTF-8");
    free(t);
    const uint8_t u16[] = {0xFF, 0xFE, 'H', 0, 0x93, 0x03, '\r', 0, '\n', 0};
    t = text_to_utf8(u16, sizeof(u16), ENC_AUTO, &len, &used);
    CHECK(t && !strcmp(t, "H\xce\x93\n"), "UTF-16LE with BOM, CR dropped");
    free(t);
    const char *hay = "Hello \xce\x9a\xce\x8c\xce\xa3\xce\x9c\xce\x95 world, hello"; /* "ΚΌΣΜΕ" */
    int hl = (int)strlen(hay);
    CHECK(text_find(hay, hl, 0, "HELLO") == 0, "find, any case");
    CHECK(text_find(hay, hl, 1, "hello") == hl - 5, "find from an offset");
    CHECK(text_find(hay, hl, 0, "\xce\xba\xce\xbf\xcf\x83\xce\xbc\xce\xb5") == 6, "Greek without accents"); /* "κοσμε" */
    CHECK(text_find(hay, hl, 0, "\xcf\x82\xce\xbc") == 10, "final sigma matches sigma"); /* "ςμ" */
    CHECK(text_find(hay, hl, 0, "xyz") == -1, "no match");
}

static int dump(int argc, char **argv)
{
    for (int i = 2; i < argc; i++) {
        int err;
        PdfDoc *d = pdf_open(argv[i], &err);
        printf("=== %s: %s\n", argv[i], d ? "ok" : "error");
        if (!d) {
            printf("error %d\n", err);
            continue;
        }
        for (int p = 0; p < pdf_page_count(d); p++) {
            PdfPageInfo info;
            int n = pdf_page_text(d, p, 0, buf, sizeof(buf), &info);
            printf("--- page %d/%d (%d bytes, undecodable %d, image %ux%u @%u)\n%s\n", p + 1, pdf_page_count(d), n,
                   info.undecodable, info.img_w, info.img_h, info.img_off, n > 0 ? buf : "");
        }
        pdf_close(d);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "dump"))
        return dump(argc, argv);
    const char *dir = argc > 1 ? argv[1] : "build/pdf";
    test_textenc();

    int err;
    PdfDoc *d = open_fx(dir, "simple.pdf", &err);
    CHECK(d && pdf_page_count(d) == 2, "simple: open (%d)", err);
    if (d) {
        expect_page(d, 0, 0, "Hello World\nKerned words\nLeft Right", "simple");
        expect_page(d, 1, 0, "Caf\xc3\xa9 na\xc3\xafve (ok)", "simple");
        pdf_close(d);
    }

    d = open_fx(dir, "modern.pdf", &err);
    CHECK(d && pdf_page_count(d) == 1, "modern: open (%d)", err);
    if (d) /* the second line is 40 units down at size 10: a paragraph break */
        expect_page(d, 0, 0, "\xce\x93\xce\xb5\xce\xb9\xce\xac \xcf\x83\xce\xbf\xcf\x85\n\nfi", "modern");
    pdf_close(d);

    d = open_fx(dir, "differences.pdf", &err);
    CHECK(d != NULL, "differences: open (%d)", err);
    if (d) {
        expect_page(d, 0, 0, "\xce\xb1\xce\xb2\xce\xb3\n\xce\xb1\xce\xb2\xce\xb3\n\xc3\x81\xc3\xa2", "differences");
        expect_page(d, 0, 1, "\xce\xb1\xce\xb2\xce\xb3\n\xce\xb1\xce\xb2\xce\xb3\n\xce\x91\xce\xb2", "legacy greek");
        pdf_close(d);
    }

    d = open_fx(dir, "scan.pdf", &err);
    CHECK(d != NULL, "scan: open (%d)", err);
    if (d) {
        PdfPageInfo info;
        int n = pdf_page_text(d, 0, 0, buf, sizeof(buf), &info);
        CHECK(n == 0, "scan: no text (%d)", n);
        CHECK(info.img_w == 64 && info.img_h == 48 && info.img_len > 100, "scan: image %dx%d len %u", info.img_w,
              info.img_h, info.img_len);
        char p[512];
        snprintf(p, sizeof(p), "%s/scan.pdf", dir);
        FILE *f = fopen(p, "rb");
        unsigned char m[2] = {0};
        if (f) {
            fseek(f, info.img_off, SEEK_SET);
            if (fread(m, 1, 2, f) != 2) m[0] = 0;
            fclose(f);
        }
        CHECK(m[0] == 0xFF && m[1] == 0xD8, "scan: image offset points at a JPEG");
        pdf_close(d);
    }

    const char *broken[] = {"broken.pdf", "broken2.pdf"};
    for (int i = 0; i < 2; i++) {
        d = open_fx(dir, broken[i], &err);
        CHECK(d && pdf_page_count(d) == 2, "%s: rescanned (%d)", broken[i], err);
        if (d)
            expect_page(d, 1, 0, "Caf\xc3\xa9 na\xc3\xafve (ok)", broken[i]);
        pdf_close(d);
    }

    d = open_fx(dir, "encrypted.pdf", &err);
    CHECK(!d && err == PDF_E_ENCRYPTED, "encrypted: refused (%d)", err);
    pdf_close(d);
    d = open_fx(dir, "notpdf.pdf", &err);
    CHECK(!d && err == PDF_E_FORMAT, "not a PDF (%d)", err);
    d = open_fx(dir, "missing.pdf", &err);
    CHECK(!d && err == PDF_E_READ, "missing file (%d)", err);

    if (fails)
        return 1;
    printf("test_pdf: all passed\n");
    return 0;
}
