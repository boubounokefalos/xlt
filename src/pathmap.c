/* XLT - a small remembered number per file. See pathmap.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pathmap.h"

static int find(const PathMap *m, const char *path)
{
    for (int i = 0; i < m->n; i++)
        if (!strcmp(m->e[i].path, path))
            return i;
    return -1;
}

int pm_get(const PathMap *m, const char *path)
{
    int i = find(m, path);
    return i >= 0 ? m->e[i].value : 0;
}

void pm_set(PathMap *m, const char *path, int value)
{
    int i = find(m, path);
    if (i >= 0) { /* out of its place */
        memmove(&m->e[i], &m->e[i + 1], sizeof(m->e[0]) * (m->n - i - 1));
        m->n--;
    }
    if (!value || strlen(path) >= sizeof(m->e[0].path))
        return;
    if (m->n == PM_MAX)
        m->n--; /* the oldest goes */
    memmove(&m->e[1], &m->e[0], sizeof(m->e[0]) * m->n);
    snprintf(m->e[0].path, sizeof(m->e[0].path), "%s", path);
    m->e[0].value = value;
    m->n++;
}

void pm_parse(PathMap *m, const char *text, int len)
{
    m->n = 0;
    int i = 0;
    while (i < len && m->n < PM_MAX) {
        int s = i;
        while (i < len && text[i] != '\n') i++;
        int n = i - s;
        i++;
        if (n > 0 && text[s + n - 1] == '\r') n--;
        char line[230];
        if (n <= 0 || n >= (int)sizeof(line))
            continue;
        memcpy(line, text + s, n);
        line[n] = 0;
        char *tab = strchr(line, '\t');
        if (!tab || !tab[1] || strlen(tab + 1) >= sizeof(m->e[0].path))
            continue;
        int v = atoi(line);
        if (v && find(m, tab + 1) < 0) {
            snprintf(m->e[m->n].path, sizeof(m->e[0].path), "%s", tab + 1);
            m->e[m->n].value = v;
            m->n++;
        }
    }
}

int pm_write(const PathMap *m, char *out, int cap)
{
    int k = 0;
    if (cap > 0) out[0] = 0;
    for (int i = 0; i < m->n; i++) {
        int n = snprintf(out + k, cap - k, "%d\t%s\n", m->e[i].value, m->e[i].path);
        if (n < 0 || n >= cap - k)
            return -1;
        k += n;
    }
    return k;
}
