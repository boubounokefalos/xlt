/* XLT - background music player: MP3 and AAC (.m4a, .aac) on the PSP's own
 * decoders, FLAC on the CPU. Music keeps playing while you browse; the next
 * track starts by itself. */
#ifndef MUSIC_H
#define MUSIC_H

#include "entry.h"

enum { MUSIC_STOPPED, MUSIC_PLAYING, MUSIC_PAUSED };

/* Plays the Music view from track `index` (the list is copied, so a rescan
 * doesn't disturb it). Loads the decoder modules the first time. */
void music_play_view(int index);
/* Plays a list of song paths (a folder in the file manager) from `index`. */
void music_play_files(const char *const *paths, int n, int index);
/* Plays a list of songs whose tags are known already (a playlist) from `index`. */
void music_play_entries(const Entry *list, int n, int index);
void music_toggle(void);      /* play/pause */
void music_next(void);
void music_prev(void);        /* restarts the track if more than 3 s in, else the previous one */
void music_seek(int delta_ms);
void music_stop(void);
/* Stops playback and frees the decoder, audio channel and modules (before a
 * launch, USB mode or exit). Playing again later reloads them. */
void music_shutdown(void);

/* Reads where playback was last time (cache/music.txt) and the shuffle and
 * repeat switches. Before anything else here. */
void music_init(const char *app_dir);
/* UI thread, every frame: saves where playback is when that changes. */
void music_poll(void);
/* 1 if nothing plays and there is a place to go back to (title/artist may be NULL). */
int music_can_resume(const char **title, const char **artist);
/* Plays the list from last time again, from that song, at that time. -1 if it can't. */
int music_resume(void);

/* Shuffle: songs in a mixed order (the playing one stays). Repeat: REPEAT_* (resume.h). */
void music_set_shuffle(int on);
int music_shuffle(void);
void music_set_repeat(int mode);
int music_repeat(void);
/* The song `ahead` places after the playing one in the play order. -1 if none. */
int music_peek(int ahead, const char **title, const char **artist);

/* The sleep timer: off -> 15 -> 30 -> 45 -> 60 -> 90 min -> off; returns the new
 * setting. The music fades out over the last 10 s and stops (its place kept). */
int music_sleep_cycle(void);
int music_sleep_left(void);    /* minutes left (rounded up), 0 = off */

int music_state(void);
/* What is playing: returns 0 and fills the fields if a track is loaded. */
typedef struct {
    const char *title, *artist, *path;
    int index, count;          /* position in the play order of the copied list */
    int elapsed_ms, duration_ms; /* duration 0 if unknown */
} MusicNow;
int music_now(MusicNow *out);
/* Changes whenever the track or the play state changes (for redraws). */
int music_generation(void);

#endif
