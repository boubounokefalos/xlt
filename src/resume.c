/* XLT - the music player's saved place. See resume.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "resume.h"

/* The value of "key=" at the start of a line, copied into out; 0 if absent. */
static int get(const char *text, const char *key, char *out, int n)
{
    int kl = (int)strlen(key);
    for (const char *l = text; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL) {
        if (!strncmp(l, key, kl) && l[kl] == '=') {
            const char *v = l + kl + 1;
            int i = 0;
            while (v[i] && v[i] != '\n' && v[i] != '\r' && i < n - 1) { out[i] = v[i]; i++; }
            out[i] = 0;
            return 1;
        }
    }
    return 0;
}

void resume_parse_state(const char *text, ResumeState *st)
{
    char v[128];
    memset(st, 0, sizeof(*st));
    st->track = -1;
    if (get(text, "track", v, sizeof(v))) st->track = atoi(v) >= 0 ? atoi(v) : -1;
    if (get(text, "ms", v, sizeof(v))) st->ms = atoi(v) > 0 ? atoi(v) : 0;
    if (get(text, "shuffle", v, sizeof(v))) st->shuffle = atoi(v) != 0;
    if (get(text, "repeat", v, sizeof(v))) {
        int r = atoi(v);
        st->repeat = r >= 0 && r < REPEAT_COUNT ? r : REPEAT_OFF;
    }
    get(text, "title", st->title, sizeof(st->title));
    get(text, "artist", st->artist, sizeof(st->artist));
}

/* Copies s with newlines and tabs as spaces. */
static void flat(char *out, int n, const char *s)
{
    int i = 0;
    for (; s && s[i] && i < n - 1; i++)
        out[i] = s[i] == '\n' || s[i] == '\r' || s[i] == '\t' ? ' ' : s[i];
    out[i] = 0;
}

int resume_write_state(char *out, int cap, const ResumeState *st)
{
    char t[128], a[48];
    flat(t, sizeof(t), st->title);
    flat(a, sizeof(a), st->artist);
    int n = snprintf(out, cap, "track=%d\nms=%d\nshuffle=%d\nrepeat=%d\ntitle=%s\nartist=%s\n",
                     st->track, st->ms, st->shuffle, st->repeat, t, a);
    return n < cap ? n : -1;
}

int resume_write_song(char *out, int cap, const char *path, const char *title, const char *artist)
{
    char p[256], t[128], a[48];
    flat(p, sizeof(p), path);
    flat(t, sizeof(t), title);
    flat(a, sizeof(a), artist);
    int n = snprintf(out, cap, "%s\t%s\t%s\n", p, t, a);
    return n < cap ? n : -1;
}

int resume_parse_list(const char *text, int len, ResumeSongFn fn, void *ctx)
{
    char line[512];
    int count = 0, i = 0;
    while (i < len) {
        int s = i;
        while (i < len && text[i] != '\n') i++;
        int n = i - s;
        i++;
        if (n > 0 && text[s + n - 1] == '\r') n--;
        if (n <= 0 || n >= (int)sizeof(line))
            continue;
        memcpy(line, text + s, n);
        line[n] = 0;
        char *t = strchr(line, '\t'), *a = t ? strchr(t + 1, '\t') : NULL;
        if (t) *t++ = 0;
        if (a) *a++ = 0;
        if (!line[0])
            continue;
        if (fn) fn(line, t ? t : "", a ? a : "", ctx);
        count++;
    }
    return count;
}

/* ------------------------------------------------------- play order */

static unsigned next_rand(unsigned *seed)
{
    *seed = *seed * 1103515245u + 12345u; /* small and the same on every machine (tests) */
    return *seed >> 8;
}

void play_order(int *order, int n, int shuffle, int first, unsigned *seed)
{
    for (int i = 0; i < n; i++)
        order[i] = i;
    if (!shuffle || n < 2)
        return;
    if (first < 0 || first >= n) first = 0;
    order[0] = first; /* the chosen song stays first, the rest are mixed behind it */
    order[first] = 0;
    for (int i = n - 1; i > 1; i--) {
        int j = 1 + (int)(next_rand(seed) % (unsigned)i); /* 1..i */
        int t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
}

int order_next(int pos, int n, int repeat, int ended)
{
    if (n <= 0)
        return -1;
    if (repeat == REPEAT_ONE && ended)
        return pos;
    if (pos + 1 < n)
        return pos + 1;
    return repeat == REPEAT_OFF ? -1 : 0; /* repeat one, skipped by hand: round the list too */
}

int order_prev(int pos, int n, int repeat)
{
    if (pos > 0)
        return pos - 1;
    return repeat != REPEAT_OFF && n > 0 ? n - 1 : 0;
}
