/* XLT - web page layout: html.c's item stream wrapped into lines of spans
 * (text, images, form controls) for one content width, plus the focusable
 * regions (links and controls) in reading order. Portable (host-tested). */
#ifndef WEBLAY_H
#define WEBLAY_H

#include "html.h"

enum { WS_TEXT, WS_ALT, WS_IMG, WS_CTRL, WS_RULE, WS_BULLET, WS_PREBG, WS_QUOTEBAR };
enum { WC_TEXT, WC_LINK, WC_MUTED, WC_HEAD };
enum { WSF_STRIKE = 1, WSF_CODE = 2, WSF_PLACEHOLDER = 4 /* WS_IMG not loaded yet */ };

typedef struct {
    int y;              /* top of its line */
    short x, w, h;      /* h: the line's height (contents sit on its bottom) */
    short ih;           /* WS_IMG / WS_CTRL: the box's own height */
    uint8_t kind, font, color, flags;
    int link;           /* -1 if none */
    int focus;          /* index into focus, -1 if not focusable */
    int a, b;           /* WS_TEXT, WS_BULLET: text pool offset, length; WS_ALT: strs offset, length;
                           WS_IMG: image index; WS_CTRL: control index */
} WebSpan;

typedef struct {
    int y0, y1;         /* top of the first line, bottom of the last */
    int first;          /* its first span */
    int link, ctrl;     /* one of them is -1 */
} WebFocus;

enum { WI_PENDING, WI_READY, WI_NONE /* failed, or pictures are off */ };
typedef struct { short w, h; char state; } WebImgState;

typedef struct {
    int width;                     /* content width in px */
    int font_body, font_bold, font_small;
    int line_h[8];                 /* by font id */
    int (*adv)(int font, const char **s); /* the advance of the character at *s, moving past it */
    const WebImgState *imgs;       /* per image of the document, or NULL (all pending) */
} WebLayCfg;

typedef struct {
    WebSpan *spans;
    int nspans;
    WebFocus *focus;
    int nfocus;
    int *anchor_y;                 /* per anchor of the document */
    int height;
} WebLayout;

/* NULL only when out of memory. */
WebLayout *weblay_run(const HtmlDoc *d, const WebLayCfg *cfg);
void weblay_free(WebLayout *l);

/* Index of the first span whose line reaches below y (spans are in line order). */
int weblay_span_at(const WebLayout *l, int y);

/* The size a control is drawn at (also used to draw it). */
void weblay_ctrl_size(const HtmlDoc *d, int ctrl, const WebLayCfg *cfg, int avail, int *w, int *h);
/* The size an image gets: its decoded size if ready, else from the tag, fitted into avail. */
void weblay_img_size(const HtmlDoc *d, int img, const WebLayCfg *cfg, int avail, int *w, int *h);

#endif
