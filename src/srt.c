/* XLT - SubRip subtitles. See srt.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "srt.h"
#include "textenc.h"

/* "01:02:03,456" (or with a dot) -> ms; *p is moved past it. -1 if it isn't one. */
static int timestamp(const char **p)
{
    const char *s = *p;
    int v[4] = {0, 0, 0, 0}, k = 0;
    while (*s == ' ') s++;
    for (; k < 4; k++) {
        if (*s < '0' || *s > '9') return -1;
        int digits = 0;
        while (*s >= '0' && *s <= '9') { v[k] = v[k] * 10 + (*s - '0'); s++; digits++; }
        if (k == 3) { /* milliseconds: "4" means 400 */
            while (digits < 3) { v[3] *= 10; digits++; }
            while (digits > 3) { v[3] /= 10; digits--; }
            break;
        }
        if (k < 2 ? *s != ':' : (*s != ',' && *s != '.')) return -1;
        s++;
    }
    *p = s;
    return ((v[0] * 60 + v[1]) * 60 + v[2]) * 1000 + v[3];
}

/* Takes the tags out of a line, in place: <i>, </font ...>, {\an8}. */
static void untag(char *s)
{
    char *o = s;
    for (char *p = s; *p;) {
        char close = *p == '<' ? '>' : (*p == '{' && p[1] == '\\') ? '}' : 0;
        char *e = close ? strchr(p, close) : NULL;
        if (e) { p = e + 1; continue; }
        *o++ = *p++;
    }
    *o = 0;
}

/* 1 if the line at p (up to '\n' or end) is a timing line "a --> b". */
static int is_timing(const char *p, const char *end)
{
    char tmp[64];
    int n = 0;
    while (p + n < end && p[n] != '\n' && n < (int)sizeof(tmp) - 1) { tmp[n] = p[n]; n++; }
    tmp[n] = 0;
    const char *q = tmp;
    return timestamp(&q) >= 0 && strstr(q, "-->") != NULL;
}

/* 1 if the line at p is only a cue number. */
static int is_number(const char *p)
{
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') p++;
    while (*p == ' ') p++;
    return *p == 0;
}

static int by_start(const void *a, const void *b)
{
    const SrtCue *x = a, *y = b;
    return x->start != y->start ? (x->start < y->start ? -1 : 1) : (x < y ? -1 : 1);
}

int srt_parse(Srt *s, const char *data, int len)
{
    memset(s, 0, sizeof(*s));
    const uint8_t *u = (const uint8_t *)data;
    int u16 = len >= 2 && ((u[0] == 0xFF && u[1] == 0xFE) || (u[0] == 0xFE && u[1] == 0xFF));
    int enc = u16 || utf8_valid(u, len) ? ENC_UTF8 : enc_guess_8bit(u, len);
    int n = 0;
    char *t = text_to_utf8(u, len, enc, &n, NULL);
    if (!t)
        return 0;
    s->buf = t;
    int cap = 64;
    s->cue = malloc(sizeof(SrtCue) * cap);
    if (!s->cue) { srt_free(s); return 0; }
    /* Walk the lines: a timing line starts a cue, its text runs to a blank line. */
    char *p = t, *end = t + n;
    while (p < end) {
        char *nl = memchr(p, '\n', end - p);
        char *line_end = nl ? nl : end;
        *line_end = 0;
        const char *q = p;
        int a = timestamp(&q);
        while (*q == ' ') q++;
        int b = -1;
        if (a >= 0 && !strncmp(q, "-->", 3)) {
            q += 3;
            b = timestamp(&q);
        }
        p = nl ? nl + 1 : end;
        if (a < 0 || b < 0)
            continue; /* a number, junk, or text outside a cue */
        /* the text: up to a blank line (or the next timing line, if the blank is missing) */
        char *text = p, *w = p;
        while (p < end) {
            char *nl2 = memchr(p, '\n', end - p);
            char *le = nl2 ? nl2 : end;
            *le = 0;
            int is_blank = 1;
            for (const char *c = p; *c; c++) if (*c != ' ' && *c != '\t') { is_blank = 0; break; }
            if (is_blank) { p = nl2 ? nl2 + 1 : end; break; }
            /* the blank line is missing: the next cue's timing (or its number, then timing) ends this one */
            int next_time = is_timing(p, le) || (is_number(p) && nl2 && is_timing(nl2 + 1, end));
            if (next_time) { if (nl2) *le = '\n'; break; } /* left for the outer loop */
            untag(p);
            int ln = (int)strlen(p);
            if (w != text) *w++ = '\n';
            memmove(w, p, ln);
            w += ln;
            p = nl2 ? nl2 + 1 : end;
        }
        *w = 0;
        while (w > text && (w[-1] == ' ' || w[-1] == '\n')) *--w = 0;
        if (!text[0] || b <= a)
            continue;
        if (s->n == cap) {
            SrtCue *c = realloc(s->cue, sizeof(SrtCue) * cap * 2);
            if (!c) break;
            s->cue = c;
            cap *= 2;
        }
        s->cue[s->n].start = a;
        s->cue[s->n].end = b;
        s->cue[s->n].text = text;
        s->n++;
    }
    qsort(s->cue, s->n, sizeof(SrtCue), by_start);
    if (!s->n) srt_free(s);
    return s->n;
}

void srt_free(Srt *s)
{
    free(s->cue);
    free(s->buf);
    memset(s, 0, sizeof(*s));
}

const SrtCue *srt_at(const Srt *s, int ms)
{
    /* the last cue starting at or before ms, then a few before it (they may overlap) */
    int lo = 0, hi = s->n - 1, i = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s->cue[mid].start <= ms) { i = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    for (int k = i; k >= 0 && k > i - 4; k--)
        if (ms < s->cue[k].end)
            return &s->cue[k];
    return NULL;
}
