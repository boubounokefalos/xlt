/* srt.c: SubRip subtitles, in the encodings people's files come in. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "srt.h"

static void expect(const Srt *s, int ms, const char *want)
{
    const SrtCue *c = srt_at(s, ms);
    if (!want) {
        if (c) printf("at %d: expected nothing, got '%s'\n", ms, c->text);
        assert(!c);
        return;
    }
    if (!c || strcmp(c->text, want)) printf("at %d: '%s', want '%s'\n", ms, c ? c->text : "(none)", want);
    assert(c && !strcmp(c->text, want));
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    Srt s;
    /* UTF-8 with a BOM, CRLF, tags, two lines, a dot for the comma, a missing blank line */
    const char a[] =
        "\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:03,500\r\n<i>Hello</i> there\r\n\r\n"
        "2\r\n00:00:04.2 --> 00:00:06,000 X1:10 X2:20\r\n{\\an8}Top line\r\n<font color=\"#fff\">second</font>\r\n"
        "3\r\n00:01:00,000 --> 00:01:02,000\r\n\xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1\r\n\r\n"
        "4\r\n00:00:10,000 --> 00:00:09,000\r\nbackwards: dropped\r\n\r\n"
        "garbage line\r\n";
    assert(srt_parse(&s, a, sizeof(a) - 1) == 3);
    expect(&s, 500, NULL);
    expect(&s, 1000, "Hello there");
    expect(&s, 3499, "Hello there");
    expect(&s, 3500, NULL);
    expect(&s, 4200, "Top line\nsecond");
    expect(&s, 60500, "\xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1");
    expect(&s, 99999, NULL);
    srt_free(&s);

    /* Greek in Windows-1253, out of order, overlapping */
    const char g[] =
        "2\n00:00:05,000 --> 00:00:08,000\n\xE3\xE5\xE9\xDC \xF3\xEF\xF5 \xEA\xFC\xF3\xEC\xE5\n\n"   /* γειά σου κόσμε */
        "1\n00:00:01,000 --> 00:00:06,000\n\xCA\xE1\xEB\xE7\xEC\xDD\xF1\xE1 \xF3\xE5 \xFC\xEB\xEF\xF5\xF2\n\n"; /* Καλημέρα σε όλους */
    assert(srt_parse(&s, g, sizeof(g) - 1) == 2);
    expect(&s, 2000, "\xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1 \xCF\x83\xCE\xB5 \xCF\x8C\xCE\xBB\xCE\xBF\xCF\x85\xCF\x82");
    expect(&s, 7000, "\xCE\xB3\xCE\xB5\xCE\xB9\xCE\xAC \xCF\x83\xCE\xBF\xCF\x85 \xCE\xBA\xCF\x8C\xCF\x83\xCE\xBC\xCE\xB5");
    srt_free(&s);

    /* Western in Windows-1252: an accent here and there stays Latin */
    const char w[] = "1\n00:00:01,000 --> 00:00:02,000\nCaf\xE9 cr\xE8me, \xE7" "a va bien merci beaucoup\n";
    assert(srt_parse(&s, w, sizeof(w) - 1) == 1);
    expect(&s, 1500, "Caf\xC3\xA9 cr\xC3\xA8me, \xC3\xA7" "a va bien merci beaucoup");
    srt_free(&s);

    /* UTF-16 LE with its mark */
    const unsigned char u16[] = {0xFF, 0xFE, '1', 0, '\n', 0, '0', 0, '0', 0, ':', 0, '0', 0, '0', 0, ':', 0, '0', 0, '1', 0,
                                 ',', 0, '0', 0, '0', 0, '0', 0, ' ', 0, '-', 0, '-', 0, '>', 0, ' ', 0, '0', 0, '0', 0, ':', 0,
                                 '0', 0, '0', 0, ':', 0, '0', 0, '2', 0, ',', 0, '0', 0, '0', 0, '0', 0, '\n', 0, 'H', 0, 'i', 0,
                                 0xB1, 0x03, '\n', 0};
    assert(srt_parse(&s, (const char *)u16, sizeof(u16)) == 1);
    expect(&s, 1500, "Hi\xCE\xB1");
    srt_free(&s);

    assert(srt_parse(&s, "not subtitles at all", 20) == 0 && !s.cue && !s.buf);
    assert(srt_parse(&s, "", 0) == 0);
    puts("test_srt: ok");
    return 0;
}
