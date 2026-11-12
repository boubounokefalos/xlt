/* XLT - where each video was stopped. See vplace.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vplace.h"

void vp_parse(VPlaces *v, const char *text, int len)
{
    v->n = 0;
    int i = 0;
    while (i < len && v->n < VP_MAX) {
        int s = i;
        while (i < len && text[i] != '\n') i++;
        int n = i - s;
        i++;
        if (n > 0 && text[s + n - 1] == '\r') n--;
        char line[260];
        if (n <= 0 || n >= (int)sizeof(line))
            continue;
        memcpy(line, text + s, n);
        line[n] = 0;
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2 || !t2[1] || strlen(t2 + 1) >= sizeof(v->p[0].path))
            continue;
        VPlace *p = &v->p[v->n];
        p->ms = atoi(line);
        p->dur = atoi(t1 + 1);
        snprintf(p->path, sizeof(p->path), "%s", t2 + 1);
        if (p->ms > 0 && vp_get(v, p->path, NULL) == 0) /* no duplicates */
            v->n++;
    }
}

int vp_write(const VPlaces *v, char *out, int cap)
{
    int k = 0;
    if (cap > 0) out[0] = 0;
    for (int i = 0; i < v->n; i++) {
        int n = snprintf(out + k, cap - k, "%d\t%d\t%s\n", v->p[i].ms, v->p[i].dur, v->p[i].path);
        if (n < 0 || n >= cap - k)
            return -1;
        k += n;
    }
    return k;
}

static int find(const VPlaces *v, const char *path)
{
    for (int i = 0; i < v->n; i++)
        if (!strcmp(v->p[i].path, path))
            return i;
    return -1;
}

int vp_get(const VPlaces *v, const char *path, int *dur)
{
    int i = find(v, path);
    if (dur) *dur = i >= 0 ? v->p[i].dur : 0;
    return i >= 0 ? v->p[i].ms : 0;
}

void vp_forget(VPlaces *v, const char *path)
{
    int i = find(v, path);
    if (i < 0)
        return;
    memmove(&v->p[i], &v->p[i + 1], sizeof(VPlace) * (v->n - i - 1));
    v->n--;
}

void vp_set(VPlaces *v, const char *path, int ms, int dur)
{
    vp_forget(v, path);
    if (ms < 15000 || strlen(path) >= sizeof(v->p[0].path))
        return; /* barely started */
    if (dur > 0 && (ms > dur - 60000 || ms > dur / 10 * 9))
        return; /* as good as watched */
    if (v->n == VP_MAX)
        v->n--; /* the oldest goes */
    memmove(&v->p[1], &v->p[0], sizeof(VPlace) * v->n);
    snprintf(v->p[0].path, sizeof(v->p[0].path), "%s", path);
    v->p[0].ms = ms;
    v->p[0].dur = dur;
    v->n++;
}
