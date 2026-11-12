/* XLT - play history (<app>/cache/history.bin), keyed by file path. */
#ifndef HISTORY_H
#define HISTORY_H

#include <stdint.h>

void history_init(const char *app_dir); /* loads the file */

/* Last launch as an RTC tick (microseconds), 0 if never played. */
uint64_t history_played(const char *path);

/* Records a launch now and writes the file (called right before LoadExec). */
void history_record(const char *path);

/* Forgets every launch (the Recent rows and "Last played" start over) and deletes the file. */
void history_clear(void);
/* How many games and apps it remembers. */
int history_count(void);

/* "Played today", "Played 3 days ago", "Never played"... */
void history_describe(const char *path, char *out, int n);

#endif
