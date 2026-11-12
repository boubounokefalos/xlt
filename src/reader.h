/* XLT - full-screen reader for text files (any encoding), PDFs (their text,
 * or the page picture of a scanned one) and anything else as hex. */
#ifndef READER_H
#define READER_H

#include "system.h"

enum { READER_AUTO, READER_TEXT, READER_HEX, READER_PDF };

void reader_init(const char *app_dir); /* where the reading positions are kept */
void reader_open(const char *path, int mode);
int reader_is_open(void);   /* open or still fading */
int reader_covers(void);    /* fully opaque: nothing underneath needs drawing */
int reader_input(unsigned int pressed);
int reader_step(void);      /* 1 if a redraw is needed */
void reader_draw(const SysStatus *st);
/* Closes the document now (before a launch, USB mode or exit). */
void reader_shutdown(void);

#endif
