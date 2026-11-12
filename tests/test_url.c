/* Host test for url.c: RFC 3986 section 5.4 examples, the address bar guess, encoding. */
#include <stdio.h>
#include <string.h>
#include "../src/url.h"

static int fails;

static void eq(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want)) {
        printf("FAIL %s: got '%s' want '%s'\n", what, got, want);
        fails++;
    }
}

static void res(const char *base, const char *ref, const char *want)
{
    char out[URL_MAX];
    if (url_resolve(base, ref, out, sizeof(out)) < 0)
        strcpy(out, "(error)");
    eq(ref, out, want);
}

int main(void)
{
    const char *b = "http://a/b/c/d;p?q";
    static const char *cases[][2] = {
        {"g:h", "g:h"}, {"g", "http://a/b/c/g"}, {"./g", "http://a/b/c/g"}, {"g/", "http://a/b/c/g/"},
        {"/g", "http://a/g"}, {"//g", "http://g/"}, {"?y", "http://a/b/c/d;p?y"}, {"g?y", "http://a/b/c/g?y"},
        {"#s", "http://a/b/c/d;p?q#s"}, {"g#s", "http://a/b/c/g#s"}, {"g?y#s", "http://a/b/c/g?y#s"},
        {";x", "http://a/b/c/;x"}, {"g;x", "http://a/b/c/g;x"}, {"", "http://a/b/c/d;p?q"},
        {".", "http://a/b/c/"}, {"./", "http://a/b/c/"}, {"..", "http://a/b/"}, {"../", "http://a/b/"},
        {"../g", "http://a/b/g"}, {"../..", "http://a/"}, {"../../", "http://a/"}, {"../../g", "http://a/g"},
        {"../../../g", "http://a/g"}, {"../../../../g", "http://a/g"}, {"/./g", "http://a/g"},
        {"/../g", "http://a/g"}, {"g.", "http://a/b/c/g."}, {".g", "http://a/b/c/.g"}, {"g..", "http://a/b/c/g.."},
        {"..g", "http://a/b/c/..g"}, {"./../g", "http://a/b/g"}, {"./g/.", "http://a/b/c/g/"},
        {"g/./h", "http://a/b/c/g/h"}, {"g/../h", "http://a/b/c/h"}, {"g;x=1/./y", "http://a/b/c/g;x=1/y"},
        {"g;x=1/../y", "http://a/b/c/y"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        res(b, cases[i][0], cases[i][1]);
    res("https://example.com", "x", "https://example.com/x");
    res("https://example.com", "", "https://example.com/");
    res("https://example.com/a", "HTTPS://Other.org", "https://Other.org/");
    res("https://example.com/a/", " b c \n", "https://example.com/a/b%20c");
    res("https://en.m.wikipedia.org/wiki/PSP", "/wiki/Sony", "https://en.m.wikipedia.org/wiki/Sony");
    res("https://example.com/a?x=1", "?y=2", "https://example.com/a?y=2");

    char out[URL_MAX], h[64];
    const char *dd = "https://lite.duckduckgo.com/lite/?q=";
    struct { const char *in; int kind; const char *want; } in[] = {
        {"example.com", 1, "https://example.com/"},
        {"  en.wikipedia.org/wiki/PSP ", 1, "https://en.wikipedia.org/wiki/PSP"},
        {"http://neverssl.com", 1, "http://neverssl.com/"},
        {"localhost:8080/x", 1, "https://localhost:8080/x"},
        {"192.168.1.10", 1, "https://192.168.1.10/"},
        {"psp homebrew", 0, "https://lite.duckduckgo.com/lite/?q=psp+homebrew"},
        {"C++ & más", 0, "https://lite.duckduckgo.com/lite/?q=C%2B%2B+%26+m%C3%A1s"},
        {"file.txt", 1, "https://file.txt/"},
        {"v1.2", 0, "https://lite.duckduckgo.com/lite/?q=v1.2"},
        {"pspdev", 0, "https://lite.duckduckgo.com/lite/?q=pspdev"},
    };
    for (unsigned i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
        int k = url_from_input(in[i].in, dd, out, sizeof(out));
        if (k != in[i].kind) { printf("FAIL kind of '%s': %d\n", in[i].in, k); fails++; }
        eq(in[i].in, out, in[i].want);
    }
    if (url_from_input("   ", dd, out, sizeof(out)) != -1) { puts("FAIL empty"); fails++; }

    url_host("https://user:pw@Sub.Example.com:8443/p?q", h, sizeof(h));
    eq("host", h, "sub.example.com");
    url_pretty("https://www.example.com/", out, sizeof(out));
    eq("pretty", out, "example.com");
    if (!url_same_page("https://a/b#x", "https://a/b#y") || url_same_page("https://a/b", "https://a/c")) {
        puts("FAIL same_page");
        fails++;
    }
    if (!url_is_web("https://a/") || url_is_web("mailto:x@y") || url_is_web("javascript:void(0)")) {
        puts("FAIL is_web");
        fails++;
    }
    char small[8] = "";
    if (url_encode_append(small, sizeof(small), "abcdefghij") != -1 || strlen(small) > 7) { puts("FAIL overflow"); fails++; }

    char un[URL_MAX] = "";
    if (url_unwrap_redirect("https://duckduckgo.com/l/?uddg=https%3A%2F%2Fwww.discoverkastoria.gr%2Fen%2F&rut=87a4", un, sizeof(un)) != 1)
        { puts("FAIL unwrap"); fails++; }
    eq("unwrap", un, "https://www.discoverkastoria.gr/en/");
    if (url_unwrap_redirect("https://duckduckgo.com/?q=x", un, sizeof(un)) || url_unwrap_redirect("https://evil.com/l/?uddg=https%3A%2F%2Fa.b", un, sizeof(un)) ||
        url_unwrap_redirect("https://duckduckgo.com/l/?uddg=javascript%3Aalert(1)", un, sizeof(un)))
        { puts("FAIL unwrap: should not"); fails++; }
    if (!url_is_svg("https://w.org/static/images/x.svg?v=2") || url_is_svg("https://a/b.png") || !url_is_svg("data:image/svg+xml;base64,AA"))
        { puts("FAIL svg"); fails++; }

    char fn[128];
    url_file_name("https://a.org/files/My%20Game%20(EU).iso?x=1", NULL, fn, sizeof(fn));
    eq("file name", fn, "My Game (EU).iso");
    url_file_name("https://a.org/get.php?id=4", "attachment; filename=\"Cool: Homebrew?.zip\"", fn, sizeof(fn));
    eq("disposition", fn, "Cool_ Homebrew_.zip");
    url_file_name("https://a.org/x", "attachment; filename*=UTF-8''%CE%B1%CE%B2.mp3", fn, sizeof(fn));
    eq("disposition utf8", fn, "\xCE\xB1\xCE\xB2.mp3");
    url_file_name("https://a.org/", NULL, fn, sizeof(fn));
    eq("no name", fn, "download");

    printf("test_url: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
