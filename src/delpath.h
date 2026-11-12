/* XLT - which folders are scanned, and what "Delete from library" removes.
 * Portable (host-tested). */
#ifndef DELPATH_H
#define DELPATH_H

enum { SCAN_DISC, SCAN_EBOOT };

typedef struct {
    const char *dir; /* under each device, e.g. "PSP/GAME" */
    int type;
    int depth;       /* subfolder levels below dir that are searched */
} ScanDir;

extern const ScanDir g_scan_dirs[];
extern const int g_scan_dir_count;

/* Puts in out what deleting the entry at path removes: the file itself for a
 * disc image, the folder that holds EBOOT.PBP for everything else.
 * Returns 1 for a folder, 0 for a file, -1 if refused: not on ms0:/ef0:, not
 * inside a scanned folder, a scanned folder itself, ".." in the path, or the
 * folder of self_path (XLT itself). */
int del_target(const char *path, int is_eboot, const char *self_path, char *out, int n);

#endif
