/* XLT - on-screen volume indicator for the volume buttons. */
#ifndef OSD_H
#define OSD_H

void osd_init(void);
/* Main thread, once per frame: polls the volume. Returns 1 if a redraw is needed. */
int osd_step(void);
/* Drawn last, over everything. */
void osd_draw(void);
/* The control center changed the volume itself: take the new value as known,
 * so it doesn't pop up the indicator. */
void osd_sync(void);

#endif
