/* XLT - timed lyrics. See lrc.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lrc.h"
#include "textenc.h"

/* "[mm:ss]", "[mm:ss.x]", "[mm:ss.xx]", "[mm:ss.xxx]" or "[mm:ss:xx]" at p: ms, and
 * *end after the ']'. -1 if it isn't a time (a tag like [ar:Someone] isn't). */
static int stamp(const char *p, const char **end)
{
    if (*p != '[') return -1;
    p++;
    int m = 0, s = 0, f = 0, fd = 0, digits = 0;
    while (*p >= '0' && *p <= '9') { m = m * 10 + (*p++ - '0'); digits++; }
    if (!digits || *p != ':') return -1;
    p++;
    digits = 0;
    while (*p >= '0' && *p <= '9') { s = s * 10 + (*p++ - '0'); digits++; }
    if (!digits) return -1;
    if (*p == '.' || *p == ':') {
        p++;
        while (*p >= '0' && *p <= '9') { if (fd < 3) { f = f * 10 + (*p - '0'); fd++; } p++; }
        while (fd < 3) { f *= 10; fd++; }
    }
    if (*p != ']') return -1;
    *end = p + 1;
    return (m * 60 + s) * 1000 + f;
}

/* Takes enhanced-LRC word times ("<00:12.34>") out of a line, in place. */
static void unword(char *s)
{
    char *o = s;
    for (char *p = s; *p;) {
        if (*p == '<') {
            char *e = strchr(p, '>');
            if (e && e - p <= 12 && p[1] >= '0' && p[1] <= '9') { p = e + 1; continue; }
        }
        *o++ = *p++;
    }
    *o = 0;
}

static int by_time(const void *a, const void *b)
{
    const LrcLine *x = a, *y = b;
    if (x->ms != y->ms) return x->ms < y->ms ? -1 : 1;
    return x->text < y->text ? -1 : x->text > y->text; /* the file's order for the same time */
}

int lrc_parse(Lrc *l, const char *data, int len)
{
    memset(l, 0, sizeof(*l));
    const uint8_t *u = (const uint8_t *)data;
    int u16 = len >= 2 && ((u[0] == 0xFF && u[1] == 0xFE) || (u[0] == 0xFE && u[1] == 0xFF));
    int n = 0;
    char *t = text_to_utf8(u, len, u16 || utf8_valid(u, len) ? ENC_UTF8 : enc_guess_8bit(u, len), &n, NULL);
    if (!t)
        return 0;
    l->buf = t;
    int cap = 64, offset = 0;
    l->line = malloc(sizeof(LrcLine) * cap);
    if (!l->line) { lrc_free(l); return 0; }
    for (char *p = t, *end = t + n; p < end;) {
        char *nl = memchr(p, '\n', end - p);
        char *le = nl ? nl : end;
        *le = 0;
        char *line = p;
        p = nl ? nl + 1 : end;
        while (*line == ' ' || *line == '\t') line++;
        if (!strncmp(line, "[offset:", 8)) { offset = atoi(line + 8); continue; }
        int times[16], nt = 0, ms;
        const char *q = line, *after;
        while (nt < 16 && (ms = stamp(q, &after)) >= 0) { times[nt++] = ms; q = after; }
        if (!nt)
            continue; /* a tag, or untimed text */
        char *text = (char *)q;
        unword(text);
        while (*text == ' ') text++;
        int tl = (int)strlen(text);
        while (tl > 0 && (text[tl - 1] == ' ' || text[tl - 1] == '\t')) text[--tl] = 0;
        for (int k = 0; k < nt; k++) {
            if (l->n == cap) {
                LrcLine *g = realloc(l->line, sizeof(LrcLine) * cap * 2);
                if (!g) break;
                l->line = g;
                cap *= 2;
            }
            l->line[l->n].ms = times[k];
            l->line[l->n].text = text;
            l->n++;
        }
    }
    for (int i = 0; i < l->n; i++) { /* a positive offset shows the lyrics sooner */
        l->line[i].ms -= offset;
        if (l->line[i].ms < 0) l->line[i].ms = 0;
    }
    qsort(l->line, l->n, sizeof(LrcLine), by_time);
    if (!l->n) lrc_free(l);
    return l->n;
}

void lrc_free(Lrc *l)
{
    free(l->line);
    free(l->buf);
    memset(l, 0, sizeof(*l));
}

int lrc_index(const Lrc *l, int ms)
{
    int lo = 0, hi = l->n - 1, i = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (l->line[mid].ms <= ms) { i = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return i;
}
