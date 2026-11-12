/* Host tool and test: parse + lay out an HTML file with stand-in font metrics and print
 * the lines as text (links in [brackets], images as {img}, controls as <ctrl>).
 *   web_dump FILE [WIDTH]        -> the page as text, and timings */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/html.h"
#include "../src/weblay.h"

static int adv(int font, const char **s)
{
    const unsigned char *u = (const unsigned char *)*s;
    int n = *u < 0x80 ? 1 : *u >= 0xF0 ? 4 : *u >= 0xE0 ? 3 : 2;
    *s += n;
    return font == 2 ? 8 : font == 0 ? 6 : 7;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) return 2;
    fclose(f);
    int width = argc > 2 ? atoi(argv[2]) : 460;
    char cs[32];
    html_sniff_charset(b, (int)n, cs, sizeof(cs));
    clock_t t0 = clock();
    HtmlDoc *d = html_parse(b, (int)n);
    clock_t t1 = clock();
    WebLayCfg c = {0};
    c.width = width;
    c.font_small = 0; c.font_body = 1; c.font_bold = 2;
    c.line_h[0] = 18; c.line_h[1] = 20; c.line_h[2] = 20;
    c.adv = adv;
    WebLayout *l = weblay_run(d, &c);
    clock_t t2 = clock();
    printf("== title '%s' charset '%s' items %d links %d imgs %d ctrls %d forms %d anchors %d text %d trunc %d\n",
           d->title, cs, d->nitems, d->nlinks, d->nimgs, d->nctrls, d->nforms, d->nanchors, d->text_len, d->truncated);
    printf("== spans %d focus %d height %d  parse %.1f ms layout %.1f ms\n", l->nspans, l->nfocus, l->height,
           (t1 - t0) * 1000.0 / CLOCKS_PER_SEC, (t2 - t1) * 1000.0 / CLOCKS_PER_SEC);
    int y = -1;
    for (int i = 0; i < l->nspans; i++) {
        WebSpan *s = &l->spans[i];
        if (s->y != y) { printf("\n%6d %3d| %*s", s->y, s->h, s->x / 7, ""); y = s->y; }
        int lk = s->link >= 0;
        switch (s->kind) {
        case WS_TEXT: case WS_BULLET:
            printf("%s%.*s%s", lk ? "[" : "", s->b, d->text + s->a, lk ? "]" : ""); break;
        case WS_ALT: printf("(alt:%.*s)", s->b, d->strs + s->a); break;
        case WS_IMG: printf("{img %d %dx%d%s}", s->a, s->w, s->ih, lk ? " link" : ""); break;
        case WS_CTRL: { HtmlCtrl *k = &d->ctrls[s->a];
            printf("<ctrl t%d name=%s val=%s lab=%s %dx%d>", k->type, d->strs + k->name, d->strs + k->value, d->strs + k->label, s->w, s->ih); break; }
        case WS_RULE: printf("------"); break;
        case WS_PREBG: printf("(pre)"); break;
        case WS_QUOTEBAR: printf("|"); break;
        }
    }
    printf("\n");
    for (int i = 0; i < d->nforms; i++)
        printf("form %d action=%s post=%d\n", i, d->strs + d->forms[i].action, d->forms[i].post);
    weblay_free(l);
    html_free(d);
    free(b);
    return 0;
}
