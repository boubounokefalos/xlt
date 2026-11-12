/* XLT - video player: MP4 (H.264 + AAC) and PMF, full screen. */
#ifndef VIDEO_H
#define VIDEO_H

#include <stdint.h>
#include "entry.h"

void video_init(const char *app_dir); /* where video.log goes */
void video_open(const Entry *e);
int video_is_open(void);
int video_covers(void);   /* fully opaque: nothing underneath needs drawing */
int video_input(unsigned int pressed);
int video_step(void);     /* 1 if a redraw is needed (a new frame, the overlay...) */
void video_draw(void);
void video_shutdown(void); /* stops playback (before a launch, USB, exit) */
void video_on_suspend(void); /* power callback: going to sleep (stops the player) */
void video_on_resume(void);  /* power callback: awake (the player reopens at its place) */
/* Where a video was stopped last time (0 = not started, or watched); *dur_ms its length. */
int video_place(const char *path, int *dur_ms);

/* Muted previews in a box (the focused tile or the list's preview), looping:
 * a library video from 10% in (its first 30 s), or a game's ICON1.PMF given
 * in memory (taken over and freed by the player; key names it). Starting a new
 * one stops the old one without waiting; video_open() stops it for real. */
void video_preview_file(const char *path);
void video_preview_pmf(const char *key, uint8_t *data, uint32_t len);
void video_preview_stop(int wait); /* wait: until the decoder is free (before music starts) */
int video_preview_step(void);
void video_note(const char *fmt, ...); /* one line in cache/video.log */      /* UI thread, every frame: 1 if a new picture arrived */
const char *video_preview_path(void); /* path or key being shown, NULL if no picture yet */
int video_preview_draw(int x, int y, int w, int h); /* 1 if a picture was drawn */

/* Thumbnails: decodes one picture 10% into a video (muted, nothing shown).
 * Only while no preview is playing. video_grab returns 0 if it started;
 * video_grab_take, every frame after: 0 still working, 1 tex holds the
 * 144x80 thumbnail, -1 it failed. */
int video_preview_idle(void);
int video_grab(const char *path);
int video_grab_take(uint8_t *tex);

#endif
