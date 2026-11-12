/* XLT - removes a file, or a folder with everything in it, from the Memory Stick. */
#ifndef FSDEL_H
#define FSDEL_H

/* 0 if it is gone (or was never there), -1 if something could not be removed. */
int fs_delete(const char *path, int is_dir);

#endif
