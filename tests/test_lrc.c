/* lrc.c: timed lyrics, in the shapes .lrc files and taggers write them. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lrc.h"

static void at(const Lrc *l, int ms, const char *want)
{
    int i = lrc_index(l, ms);
    const char *got = i >= 0 ? l->line[i].text : NULL;
    if (!want) {
        if (got) printf("at %d: expected none, got '%s'\n", ms, got);
        assert(!got);
        return;
    }
    if (!got || strcmp(got, want)) printf("at %d: '%s', want '%s'\n", ms, got ? got : "(none)", want);
    assert(got && !strcmp(got, want));
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    Lrc l;
    const char a[] =
        "\xEF\xBB\xBF[ar:Some Band]\r\n[ti:A Song]\r\n[length: 03:20]\r\n"
        "[00:12.00]First line\r\n"
        "[00:15.5]Second <00:16.00>with <00:16.50>word times\r\n"
        "[00:20.123][01:10.00]Chorus, twice\r\n"
        "[00:25:50]\r\n"                                          /* a pause */
        "untimed text is left out\r\n"
        "[00:30.00]  \xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1  \r\n"
        "[1:05]Short minutes\n";
    assert(lrc_parse(&l, a, sizeof(a) - 1) == 7);
    at(&l, 0, NULL);
    at(&l, 11999, NULL);
    at(&l, 12000, "First line");
    at(&l, 15500, "Second with word times");
    at(&l, 20123, "Chorus, twice");
    at(&l, 25500, "");
    at(&l, 30000, "\xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1");
    at(&l, 65000, "Short minutes");
    at(&l, 70000, "Chorus, twice");
    at(&l, 999999, "Chorus, twice");
    lrc_free(&l);

    /* an offset, lines out of order, Greek in Windows-1253 */
    const char g[] = "[offset:+500]\n[00:10.00]\xE4\xE5\xFD\xF4\xE5\xF1\xE7 \xE3\xF1\xE1\xEC\xEC\xDE\n[00:05.00]\xF0\xF1\xFE\xF4\xE7 \xE3\xF1\xE1\xEC\xEC\xDE \xE5\xE4\xFE\n";
    assert(lrc_parse(&l, g, sizeof(g) - 1) == 2);
    at(&l, 4500, "\xCF\x80\xCF\x81\xCF\x8E\xCF\x84\xCE\xB7 \xCE\xB3\xCF\x81\xCE\xB1\xCE\xBC\xCE\xBC\xCE\xAE \xCE\xB5\xCE\xB4\xCF\x8E");
    at(&l, 9500, "\xCE\xB4\xCE\xB5\xCF\x8D\xCF\x84\xCE\xB5\xCF\x81\xCE\xB7 \xCE\xB3\xCF\x81\xCE\xB1\xCE\xBC\xCE\xBC\xCE\xAE");
    lrc_free(&l);

    /* lyrics that aren't timed, and junk */
    assert(lrc_parse(&l, "Just words\nno times here\n", 25) == 0 && !l.line && !l.buf);
    assert(lrc_parse(&l, "[ar:x]\n[ti:y]\n", 14) == 0);
    assert(lrc_parse(&l, "", 0) == 0);
    assert(lrc_parse(&l, "[00:01.00", 9) == 0); /* not closed */
    puts("test_lrc: ok");
    return 0;
}
