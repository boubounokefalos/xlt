/* XLT - recursive delete. Runs on the background worker (bg.c).
 *
 * The directory is reopened after every removal instead of reading on while
 * entries vanish under the open handle. Entries that cannot be removed stay
 * at the front of the (stable) FAT order, so they are skipped by count. */
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>
#include "fsdel.h"

#define MAX_DEPTH 8

static int remove_tree(const char *dir, int depth)
{
    if (depth > MAX_DEPTH)
        return -1;
    int failed = 0;
    for (;;) {
        SceUID d = sceIoDopen(dir);
        if (d < 0)
            return -1;
        SceIoDirent de;
        char path[256];
        int got = 0, fits = 0, is_dir = 0, seen = 0;
        memset(&de, 0, sizeof(de));
        while (sceIoDread(d, &de) > 0) {
            const char *name = de.d_name;
            if (strcmp(name, ".") && strcmp(name, "..") && seen++ >= failed) {
                got = 1;
                fits = snprintf(path, sizeof(path), "%s/%s", dir, name) < (int)sizeof(path);
                is_dir = FIO_S_ISDIR(de.d_stat.st_mode);
                break;
            }
            memset(&de, 0, sizeof(de));
        }
        sceIoDclose(d);
        if (!got)
            break; /* only entries that could not be removed are left */
        if (!fits) {
            failed++; /* name too long for us: leave it */
            continue;
        }
        int r = is_dir ? remove_tree(path, depth + 1) : sceIoRemove(path);
        if (r < 0)
            failed++;
    }
    if (sceIoRmdir(dir) < 0)
        return -1;
    return failed ? -1 : 0;
}

int fs_delete(const char *path, int is_dir)
{
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    if (sceIoGetstat(path, &st) < 0)
        return 0; /* already gone */
    if (is_dir)
        return FIO_S_ISDIR(st.st_mode) ? remove_tree(path, 0) : -1;
    return sceIoRemove(path) < 0 ? -1 : 0;
}
