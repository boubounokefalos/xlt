/* XLT - screen recorder: what XLT shows, as an MJPEG .avi in VIDEO/ (picture
 * only, 30 frames a second at most). L+R+SELECT starts and stops it. The UI
 * thread copies each new frame; a thread below the UI turns it into a JPEG and
 * writes it, so a still screen costs almost nothing and browsing stays smooth. */
#ifndef RECORDER_H
#define RECORDER_H

/* dev: "ms0:" or "ef0:" (the first 4 characters are used). 0 if it started. */
int rec_start(const char *dev);
void rec_stop(void);           /* saves the file; nothing if not recording */
int rec_active(void);
int rec_seconds(void);         /* how long it has been recording */
/* Every frame, after drawing (drew: a new picture was drawn). 1 if the
 * indicator's time changed, or the recording stopped by itself. */
int rec_frame(int drew);
/* A message for the user once the recording has ended (saved, stick full...). 1 if there is one. */
int rec_take_message(char *out, int n);
void rec_on_suspend(void);     /* power callback: saves before the Memory Stick goes away */

#endif
