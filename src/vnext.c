/* XLT - which video plays after this one. See vnext.h. */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "vnext.h"
#include "musicsort.h"

int vnext_is_video(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".m4v") || !strcasecmp(dot, ".pmf"));
}

void vnext_consider(const char *me, const char *name, char *best, int n)
{
    if (name[0] == '.' || !vnext_is_video(name) || natural_cmp(name, me) <= 0)
        return;
    if (!best[0] || natural_cmp(name, best) < 0)
        snprintf(best, n, "%s", name);
}
