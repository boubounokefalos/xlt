/* XLT - file manager jobs on their own thread: copy, move, delete, folder
 * size and ZIP extraction, with progress and cancel. One job at a time. */
#ifndef FILEOPS_H
#define FILEOPS_H

#include <stdint.h>

#define FOP_PATH 256
#define FOP_MAX_ITEMS 256

/* FOP_EXTRACT: one ZIP into dest_dir, keeping what is there (new files get numbered names);
 * FOP_EXTRACT_OVER: replacing it. Either makes dest_dir if needed. */
enum { FOP_COPY, FOP_MOVE, FOP_DELETE, FOP_SIZE, FOP_EXTRACT, FOP_EXTRACT_OVER };
enum { FOP_IDLE, FOP_COUNTING, FOP_WORKING, FOP_DONE };

typedef struct {
    int op, phase;
    int items_done, items_total; /* files (and folders, for delete) */
    uint64_t bytes_done, bytes_total;
    char current[96];            /* name being worked on */
    int errors;
    int cancelled;
    int full;                    /* stopped: the Memory Stick is full */
    char first_error[96];
    char dest_name[96];          /* copy/move of one item: its name at the destination */
} FopStatus;

void fop_init(void);
/* Starts a job on n paths (copied). dest_dir: copy/move target folder, with
 * or without a trailing slash. Returns 0, or -1 if a job is running or the
 * request is refused (*why says why). */
int fop_start(int op, const char (*paths)[FOP_PATH], int n, const char *dest_dir, const char **why);
int fop_busy(void);
void fop_status(FopStatus *out);
void fop_cancel(void);
/* 1 once after a job finished, with its final status. */
int fop_take_result(FopStatus *out);

/* Portable helpers (also used by the file manager). */
const char *fop_basename(const char *path);
/* "name.ext" -> "name (2).ext", the first n >= 2 for which exists(dir/name) is 0. */
void fop_unique_name(const char *dir, const char *name, char *out, int outn, int (*exists)(const char *path));
/* 1 if `inner` is `outer` or inside it (so a folder isn't copied into itself). */
int fop_is_inside(const char *inner, const char *outer);

#endif
