/* XLT - HTML for the web browser: a forgiving tag-soup parser that turns a
 * page into a flat stream of items (text runs, breaks, images, form
 * controls) for weblay.c to lay out. No CSS and no JavaScript: what a page
 * looks like comes from its tags. Portable (host-tested). */
#ifndef HTML_H
#define HTML_H

#include <stdint.h>

enum {
    HI_TEXT,   /* a: offset in text pool, b: length */
    HI_BREAK,  /* <br>, or a newline in <pre> */
    HI_PARA,   /* block boundary; a: gap class HG_* (merged: the larger wins) */
    HI_CELL,   /* between two table cells on one row */
    HI_BULLET, /* a list item's marker; a, b: its text in the text pool */
    HI_IMG,    /* a: index into imgs */
    HI_CTRL,   /* a: index into ctrls */
    HI_RULE,   /* <hr> */
    HI_ANCHOR, /* a: index into anchors (a jump target: id= / name=) */
};

enum { HG_LINE, HG_SMALL, HG_PARA, HG_HEAD };

enum {
    HF_BOLD = 1,
    HF_SMALL = 2,
    HF_MUTED = 4,
    HF_PRE = 8,
    HF_CENTER = 16,
    HF_QUOTE = 32,
    HF_CODE = 64,
    HF_STRIKE = 128,
};

typedef struct {
    uint8_t kind, flags, head, indent; /* head: 1..6 inside <h1>..<h6>, else 0 */
    int link;                          /* index into links, -1 if none */
    int a, b;
} HtmlItem;

/* Strings of links, images, controls and forms live in the string pool (strs). */
typedef struct { int href; } HtmlLink;
typedef struct { int src, alt; int w, h; int link; } HtmlImg; /* w/h: from the tag, 0 if not given */

enum {
    HC_TEXT, HC_PASSWORD, HC_TEXTAREA, HC_SUBMIT, HC_BUTTON /* type=button: needs JavaScript */,
    HC_CHECKBOX, HC_RADIO, HC_SELECT, HC_HIDDEN, HC_IMAGE /* <input type=image>: a picture that submits */,
};
typedef struct {
    int type, form;     /* form: index into forms, -1 if outside any */
    int name, value;    /* value: the initial value (text, the submit label, the checkbox value) */
    int label;          /* buttons: what is written on it; text fields: the placeholder */
    int checked;        /* checkbox / radio */
    int opt_first, opt_n, sel; /* select: its options in opts, the selected one */
    int size;           /* text fields: the size= attribute, 0 if none */
} HtmlCtrl;
typedef struct { int value, label; } HtmlOpt;
typedef struct { int action, post; } HtmlForm;
typedef struct { int name, item; } HtmlAnchor;

typedef struct {
    HtmlItem *items;
    int nitems;
    char *text; /* text pool: what HI_TEXT / HI_BULLET point into */
    int text_len;
    char *strs; /* string pool: NUL-terminated attribute values */
    int strs_len;
    HtmlLink *links;   int nlinks;
    HtmlImg *imgs;     int nimgs;
    HtmlCtrl *ctrls;   int nctrls;
    HtmlOpt *opts;     int nopts;
    HtmlForm *forms;   int nforms;
    HtmlAnchor *anchors; int nanchors;
    char title[192];
    int base;        /* <base href>, -1 if none */
    int refresh;     /* <meta http-equiv=refresh> target, -1 if none */
    int refresh_sec;
    int truncated;   /* hit a size limit: the rest of the page was dropped */
} HtmlDoc;

#define HTML_STR(d, off) ((d)->strs + (off))

/* Parses len bytes of UTF-8 (need not be terminated). NULL only when out of memory. */
HtmlDoc *html_parse(const char *utf8, int len);
void html_free(HtmlDoc *d);

/* The charset a page declares in a <meta> near its start ("" if none), lower case. */
void html_sniff_charset(const char *bytes, int n, char *out, int on);

/* Decodes HTML character references in s into out (UTF-8). Exposed for tests. */
void html_decode_entities(const char *s, int len, char *out, int on);

#endif
