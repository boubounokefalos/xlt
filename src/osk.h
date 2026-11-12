/* XLT - on-screen keyboard: a panel over the dimmed screen, for names
 * (rename, new folder), search text and page numbers. Latin, Greek and
 * symbol layouts. */
#ifndef OSK_H
#define OSK_H

enum {
    OSK_FILENAME, /* no \ / : * ? " < > |, no trailing dots or spaces */
    OSK_TEXT,
    OSK_NUMBER,
    OSK_URL,      /* the browser's address bar: / and : on the letter keys */
};

#define OSK_MAX 191 /* bytes of UTF-8 */

/* text is NULL if the keyboard was cancelled. */
typedef void (*OskFn)(const char *text);

void osk_open(const char *title, const char *initial, int mode, OskFn fn);
int osk_is_open(void);
int osk_input(unsigned int pressed);
int osk_step(void);
void osk_draw(void);

#endif
