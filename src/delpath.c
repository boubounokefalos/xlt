/* XLT - scan folders and delete targets. See delpath.h. */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "delpath.h"

/* ISO/: two levels of subfolders (ARK's CAT_ folders, or your own grouping).
 * PSP/GAME/: one level of folders that have no EBOOT.PBP of their own
 * (category folders such as CAT_Emulators). */
const ScanDir g_scan_dirs[] = {
    {"ISO", SCAN_DISC, 2},
    {"PSP/GAME", SCAN_EBOOT, 1},
};
const int g_scan_dir_count = sizeof(g_scan_dirs) / sizeof(g_scan_dirs[0]);

static int has_ext(const char *s, const char *ext)
{
    size_t n = strlen(s), m = strlen(ext);
    return n > m && !strcasecmp(s + n - m, ext);
}

int del_target(const char *path, int is_eboot, const char *self_path, char *out, int n)
{
    if (strncasecmp(path, "ms0:/", 5) && strncasecmp(path, "ef0:/", 5))
        return -1;
    if (strstr(path, "/../") || strstr(path, "/./") || strstr(path, "//"))
        return -1;
    const char *rest = path + 5;
    int type = is_eboot ? SCAN_EBOOT : SCAN_DISC;
    size_t base = 0;
    for (int i = 0; i < g_scan_dir_count && !base; i++) {
        size_t m = strlen(g_scan_dirs[i].dir);
        if (g_scan_dirs[i].type == type && !strncasecmp(rest, g_scan_dirs[i].dir, m) && rest[m] == '/')
            base = 5 + m; /* length of "ms0:/PSP/GAME" */
    }
    if (!base)
        return -1;

    if (!is_eboot) {
        if (!has_ext(path, ".iso") && !has_ext(path, ".cso") && !has_ext(path, ".zso"))
            return -1;
        if ((int)strlen(path) >= n)
            return -1;
        snprintf(out, n, "%s", path);
        return 0;
    }

    const char *slash = strrchr(path, '/');
    if (!slash || strcasecmp(slash + 1, "EBOOT.PBP"))
        return -1;
    size_t len = (size_t)(slash - path); /* the folder holding EBOOT.PBP */
    if (len <= base + 1 || (int)len >= n)
        return -1;                       /* the scanned folder itself */
    if (self_path && !strncasecmp(self_path, path, len) && self_path[len] == '/')
        return -1;                       /* XLT's own folder */
    snprintf(out, n, "%.*s", (int)len, path);
    return 1;
}
