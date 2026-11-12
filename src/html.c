/* XLT - HTML tag-soup parser. See html.h.
 *
 * One pass over the bytes. Open elements sit on a stack, each remembering
 * the style that was in force before it; closing one (or having it closed
 * by an implied end, as <li> closes the previous <li>) puts that style back.
 * Text is collapsed the way a browser collapses white space and appended to
 * the current run while the style stays the same. */
#define _GNU_SOURCE /* strcasestr */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "html.h"

#define MAX_DEPTH 200
#define MAX_ITEMS 300000
#define MAX_POOL (6 << 20)
#define MAX_ATTRS 32
#define MAX_ANCHORS 12000
#define MAX_CTRLS 2000
#define MAX_OPTS 6000

/* ------------------------------------------------------------- entities */

static const struct { const char *n; unsigned cp; } s_ent[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
    {"iexcl", 0xA1}, {"cent", 0xA2}, {"pound", 0xA3}, {"curren", 0xA4}, {"yen", 0xA5}, {"brvbar", 0xA6},
    {"sect", 0xA7}, {"uml", 0xA8}, {"copy", 0xA9}, {"ordf", 0xAA}, {"laquo", 0xAB}, {"not", 0xAC},
    {"shy", 0xAD}, {"reg", 0xAE}, {"macr", 0xAF}, {"deg", 0xB0}, {"plusmn", 0xB1}, {"sup2", 0xB2},
    {"sup3", 0xB3}, {"acute", 0xB4}, {"micro", 0xB5}, {"para", 0xB6}, {"middot", 0xB7}, {"cedil", 0xB8},
    {"sup1", 0xB9}, {"ordm", 0xBA}, {"raquo", 0xBB}, {"frac14", 0xBC}, {"frac12", 0xBD}, {"frac34", 0xBE},
    {"iquest", 0xBF}, {"Agrave", 0xC0}, {"Aacute", 0xC1}, {"Acirc", 0xC2}, {"Atilde", 0xC3}, {"Auml", 0xC4},
    {"Aring", 0xC5}, {"AElig", 0xC6}, {"Ccedil", 0xC7}, {"Egrave", 0xC8}, {"Eacute", 0xC9}, {"Ecirc", 0xCA},
    {"Euml", 0xCB}, {"Igrave", 0xCC}, {"Iacute", 0xCD}, {"Icirc", 0xCE}, {"Iuml", 0xCF}, {"ETH", 0xD0},
    {"Ntilde", 0xD1}, {"Ograve", 0xD2}, {"Oacute", 0xD3}, {"Ocirc", 0xD4}, {"Otilde", 0xD5}, {"Ouml", 0xD6},
    {"times", 0xD7}, {"Oslash", 0xD8}, {"Ugrave", 0xD9}, {"Uacute", 0xDA}, {"Ucirc", 0xDB}, {"Uuml", 0xDC},
    {"Yacute", 0xDD}, {"THORN", 0xDE}, {"szlig", 0xDF}, {"agrave", 0xE0}, {"aacute", 0xE1}, {"acirc", 0xE2},
    {"atilde", 0xE3}, {"auml", 0xE4}, {"aring", 0xE5}, {"aelig", 0xE6}, {"ccedil", 0xE7}, {"egrave", 0xE8},
    {"eacute", 0xE9}, {"ecirc", 0xEA}, {"euml", 0xEB}, {"igrave", 0xEC}, {"iacute", 0xED}, {"icirc", 0xEE},
    {"iuml", 0xEF}, {"eth", 0xF0}, {"ntilde", 0xF1}, {"ograve", 0xF2}, {"oacute", 0xF3}, {"ocirc", 0xF4},
    {"otilde", 0xF5}, {"ouml", 0xF6}, {"divide", 0xF7}, {"oslash", 0xF8}, {"ugrave", 0xF9}, {"uacute", 0xFA},
    {"ucirc", 0xFB}, {"uuml", 0xFC}, {"yacute", 0xFD}, {"thorn", 0xFE}, {"yuml", 0xFF},
    {"OElig", 0x152}, {"oelig", 0x153}, {"Scaron", 0x160}, {"scaron", 0x161}, {"Yuml", 0x178}, {"fnof", 0x192},
    {"circ", 0x2C6}, {"tilde", 0x2DC},
    {"Alpha", 0x391}, {"Beta", 0x392}, {"Gamma", 0x393}, {"Delta", 0x394}, {"Epsilon", 0x395}, {"Zeta", 0x396},
    {"Eta", 0x397}, {"Theta", 0x398}, {"Iota", 0x399}, {"Kappa", 0x39A}, {"Lambda", 0x39B}, {"Mu", 0x39C},
    {"Nu", 0x39D}, {"Xi", 0x39E}, {"Omicron", 0x39F}, {"Pi", 0x3A0}, {"Rho", 0x3A1}, {"Sigma", 0x3A3},
    {"Tau", 0x3A4}, {"Upsilon", 0x3A5}, {"Phi", 0x3A6}, {"Chi", 0x3A7}, {"Psi", 0x3A8}, {"Omega", 0x3A9},
    {"alpha", 0x3B1}, {"beta", 0x3B2}, {"gamma", 0x3B3}, {"delta", 0x3B4}, {"epsilon", 0x3B5}, {"zeta", 0x3B6},
    {"eta", 0x3B7}, {"theta", 0x3B8}, {"iota", 0x3B9}, {"kappa", 0x3BA}, {"lambda", 0x3BB}, {"mu", 0x3BC},
    {"nu", 0x3BD}, {"xi", 0x3BE}, {"omicron", 0x3BF}, {"pi", 0x3C0}, {"rho", 0x3C1}, {"sigmaf", 0x3C2},
    {"sigma", 0x3C3}, {"tau", 0x3C4}, {"upsilon", 0x3C5}, {"phi", 0x3C6}, {"chi", 0x3C7}, {"psi", 0x3C8},
    {"omega", 0x3C9},
    {"ensp", 0x2002}, {"emsp", 0x2003}, {"thinsp", 0x2009}, {"zwnj", 0x200C}, {"zwj", 0x200D},
    {"lrm", 0x200E}, {"rlm", 0x200F}, {"ndash", 0x2013}, {"mdash", 0x2014}, {"lsquo", 0x2018},
    {"rsquo", 0x2019}, {"sbquo", 0x201A}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bdquo", 0x201E},
    {"dagger", 0x2020}, {"Dagger", 0x2021}, {"bull", 0x2022}, {"hellip", 0x2026}, {"permil", 0x2030},
    {"prime", 0x2032}, {"Prime", 0x2033}, {"lsaquo", 0x2039}, {"rsaquo", 0x203A}, {"euro", 0x20AC},
    {"trade", 0x2122}, {"larr", 0x2190}, {"uarr", 0x2191}, {"rarr", 0x2192}, {"darr", 0x2193},
    {"harr", 0x2194}, {"minus", 0x2212}, {"le", 0x2264}, {"ge", 0x2265}, {"ne", 0x2260}, {"asymp", 0x2248},
    {"infin", 0x221E}, {"hearts", 0x2665}, {"star", 0x2606}, {"check", 0x2713}, {"middot", 0xB7},
    {"NewLine", '\n'}, {"Tab", '\t'}, {"colon", ':'}, {"comma", ','}, {"period", '.'}, {"excl", '!'},
    {"quest", '?'}, {"num", '#'}, {"dollar", '$'}, {"percnt", '%'}, {"lpar", '('}, {"rpar", ')'},
    {"ast", '*'}, {"plus", '+'}, {"sol", '/'}, {"semi", ';'}, {"equals", '='}, {"commat", '@'},
    {"lsqb", '['}, {"rsqb", ']'}, {"lowbar", '_'}, {"grave", '`'}, {"lcub", '{'}, {"rcub", '}'},
    {"verbar", '|'}, {"vert", '|'}, {"Hat", '^'}, {"bsol", '\\'}, {"half", 0xBD},
};

/* Windows-1252's 0x80..0x9F, which pages send as &#150; and the like. */
static const unsigned short s_c1[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

/* Reads a character reference at s (just after '&'); returns its code point
 * and how many bytes it used in *used, or 0 if it isn't one. */
static unsigned entity(const char *s, const char *end, int *used)
{
    *used = 0;
    if (s < end && *s == '#') {
        const char *p = s + 1;
        unsigned v = 0;
        int digits = 0;
        if (p < end && (*p == 'x' || *p == 'X')) {
            p++;
            while (p < end && isxdigit((unsigned char)*p) && digits < 8) {
                v = v * 16 + (isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
                p++;
                digits++;
            }
        } else {
            while (p < end && isdigit((unsigned char)*p) && digits < 8) {
                v = v * 10 + (*p - '0');
                p++;
                digits++;
            }
        }
        if (!digits)
            return 0;
        if (p < end && *p == ';') p++;
        *used = (int)(p - s);
        if (v >= 0x80 && v < 0xA0 && s_c1[v - 0x80]) v = s_c1[v - 0x80];
        if (v == 0 || v > 0x10FFFF || (v >= 0xD800 && v < 0xE000)) v = 0xFFFD;
        return v;
    }
    char name[12];
    int n = 0;
    while (s + n < end && n < 11 && isalnum((unsigned char)s[n])) {
        name[n] = s[n];
        n++;
    }
    name[n] = 0;
    if (!n)
        return 0;
    /* the longest name that matches (a few, like &copy and &not, may come without ';') */
    for (int l = n; l >= 2; l--) {
        for (unsigned i = 0; i < sizeof(s_ent) / sizeof(s_ent[0]); i++) {
            if ((int)strlen(s_ent[i].n) == l && !strncmp(s_ent[i].n, name, l)) {
                int semi = s + l < end && s[l] == ';';
                if (!semi && l != n && l < 4) continue;
                *used = l + semi;
                return s_ent[i].cp;
            }
        }
    }
    return 0;
}

static int put_utf8(unsigned cp, char *o)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | (cp >> 6 & 63)); o[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | (cp >> 12 & 63));
    o[2] = (char)(0x80 | (cp >> 6 & 63)); o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

static unsigned get_utf8(const unsigned char **ps, const unsigned char *end)
{
    const unsigned char *s = *ps;
    unsigned c = *s++;
    int extra = c >= 0xF0 && c < 0xF8 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (c >= 0x80 && !extra) { *ps = s; return 0xFFFD; }
    if (extra == 3 && c >= 0xF8) extra = 0;
    unsigned cp = extra == 3 ? c & 7 : extra == 2 ? c & 15 : extra == 1 ? c & 31 : c;
    for (int i = 0; i < extra; i++) {
        if (s >= end || (*s & 0xC0) != 0x80) { *ps = s; return 0xFFFD; }
        cp = cp << 6 | (*s++ & 63);
    }
    *ps = s;
    return cp;
}

void html_decode_entities(const char *s, int len, char *out, int on)
{
    const char *end = s + len;
    int o = 0;
    while (s < end && o < on - 5) {
        if (*s == '&') {
            int used;
            unsigned cp = entity(s + 1, end, &used);
            if (used) {
                o += put_utf8(cp, out + o);
                s += 1 + used;
                continue;
            }
        }
        out[o++] = *s++;
    }
    out[o] = 0;
}

/* What the fonts can show: some characters become ones they have, some are dropped.
 * Returns the replacement in rep (NUL-terminated), "" to drop, or NULL to keep cp. */
static const char *fold(unsigned cp)
{
    if (cp == 0xAD || (cp >= 0x200B && cp <= 0x200F) || cp == 0x2060 || cp == 0xFEFF || (cp >= 0xFE00 && cp <= 0xFE0F) ||
        cp >= 0x1F000 || (cp >= 0x2600 && cp <= 0x27BF && cp != 0x2605 && cp != 0x2606 && cp != 0x2713 && cp != 0x2714) ||
        (cp >= 0xE000 && cp <= 0xF8FF) /* private use: icon fonts */)
        return "";
    switch (cp) {
    case 0x2002: case 0x2003: case 0x2004: case 0x2005: case 0x2006: case 0x2007: case 0x2008:
    case 0x2009: case 0x200A: case 0x202F: case 0x205F: case 0x3000: return " ";
    case 0x2010: case 0x2011: return "-";
    case 0x2032: return "'";
    case 0x2033: return "\"";
    case 0x2190: return "<-";
    case 0x2192: return "->";
    case 0x2191: return "^";
    case 0x2193: return "v";
    case 0x2194: return "<->";
    case 0x21D2: return "=>";
    case 0x2264: return "<=";
    case 0x2265: return ">=";
    case 0x2260: return "!=";
    case 0x2248: return "~";
    case 0x2605: case 0x2606: return "*";
    case 0x2713: case 0x2714: return "\xE2\x80\xA2"; /* a bullet */
    case 0x2715: case 0x2716: case 0x2717: case 0x2718: return "x";
    case 0x25B6: case 0x25B8: case 0x25BA: return ">";
    case 0x25C0: case 0x25C2: case 0x25C4: return "<";
    case 0x25BC: case 0x25BE: return "v";
    case 0x2039: case 0x203A: return NULL;
    case 0x02C6: return "^";
    case 0x02DC: return "~";
    case 0x2665: return "<3";
    case 0x221E: return "oo";
    }
    return NULL;
}

/* ------------------------------------------------------------------ tags */

enum {
    T_UNKNOWN, T_A, T_ABBR, T_ADDRESS, T_ARTICLE, T_ASIDE, T_AUDIO, T_B, T_BASE, T_BIG, T_BLOCKQUOTE, T_BODY, T_BR,
    T_BUTTON, T_CANVAS, T_CAPTION, T_CENTER, T_CITE, T_CODE, T_DD, T_DEL, T_DETAILS, T_DFN, T_DIALOG, T_DIV, T_DL,
    T_DT, T_EM, T_EMBED, T_FIELDSET, T_FIGCAPTION, T_FIGURE, T_FONT, T_FOOTER, T_FORM, T_FRAME, T_H1, T_H2, T_H3,
    T_H4, T_H5, T_H6, T_HEAD, T_HEADER, T_HGROUP, T_HR, T_HTML, T_I, T_IFRAME, T_IMG, T_INPUT, T_INS, T_KBD,
    T_LABEL, T_LEGEND, T_LI, T_LINK, T_MAIN, T_MAP, T_MARK, T_MATH, T_MENU, T_META, T_NAV, T_NOEMBED, T_NOFRAMES,
    T_NOSCRIPT, T_OBJECT, T_OL, T_OPTGROUP, T_OPTION, T_P, T_PARAM, T_PICTURE, T_PRE, T_Q, T_S, T_SAMP, T_SCRIPT,
    T_SECTION, T_SELECT, T_SMALL, T_SOURCE, T_SPAN, T_STRIKE, T_STRONG, T_STYLE, T_SUB, T_SUMMARY, T_SUP, T_SVG,
    T_TABLE, T_TBODY, T_TD, T_TEMPLATE, T_TEXTAREA, T_TFOOT, T_TH, T_THEAD, T_TIME, T_TITLE, T_TR, T_TRACK, T_TT,
    T_U, T_UL, T_VAR, T_VIDEO, T_WBR, T_XMP, T_AREA, T_COL, T_COLGROUP, T_DATALIST, T_PLAINTEXT, T_COUNT
};

static const char *const s_tag_names[T_COUNT] = {
    "", "a", "abbr", "address", "article", "aside", "audio", "b", "base", "big", "blockquote", "body", "br",
    "button", "canvas", "caption", "center", "cite", "code", "dd", "del", "details", "dfn", "dialog", "div", "dl",
    "dt", "em", "embed", "fieldset", "figcaption", "figure", "font", "footer", "form", "frame", "h1", "h2", "h3",
    "h4", "h5", "h6", "head", "header", "hgroup", "hr", "html", "i", "iframe", "img", "input", "ins", "kbd",
    "label", "legend", "li", "link", "main", "map", "mark", "math", "menu", "meta", "nav", "noembed", "noframes",
    "noscript", "object", "ol", "optgroup", "option", "p", "param", "picture", "pre", "q", "s", "samp", "script",
    "section", "select", "small", "source", "span", "strike", "strong", "style", "sub", "summary", "sup", "svg",
    "table", "tbody", "td", "template", "textarea", "tfoot", "th", "thead", "time", "title", "tr", "track", "tt",
    "u", "ul", "var", "video", "wbr", "xmp", "area", "col", "colgroup", "datalist", "plaintext",
};

static int tag_id(const char *name)
{
    for (int i = 1; i < T_COUNT; i++)
        if (s_tag_names[i][0] == name[0] && !strcmp(s_tag_names[i], name))
            return i;
    return T_UNKNOWN;
}

static int is_void(int t)
{
    switch (t) {
    case T_AREA: case T_BASE: case T_BR: case T_COL: case T_EMBED: case T_HR: case T_IMG: case T_INPUT:
    case T_LINK: case T_META: case T_PARAM: case T_SOURCE: case T_TRACK: case T_WBR: case T_FRAME:
        return 1;
    }
    return 0;
}

/* Elements whose content is never shown. */
static int is_skipped(int t)
{
    switch (t) {
    case T_SCRIPT: case T_STYLE: case T_TEMPLATE: case T_SVG: case T_MATH: case T_HEAD: case T_NOEMBED:
    case T_NOFRAMES: case T_AUDIO: case T_VIDEO: case T_CANVAS: case T_OBJECT: case T_MAP: case T_DATALIST:
    case T_SELECT: /* its options are collected as the control's */
    case T_TITLE: case T_TEXTAREA: case T_BUTTON: case T_OPTION: /* captured as text */
        return 1;
    }
    return 0;
}

/* Gap before and after a block element, -1 if it is inline. */
static int block_gap(int t)
{
    switch (t) {
    case T_P: case T_BLOCKQUOTE: case T_PRE: case T_FIGURE: case T_DL: case T_FIELDSET: case T_XMP:
        return HG_PARA;
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
        return HG_HEAD;
    case T_UL: case T_OL: case T_MENU: case T_TABLE: case T_DETAILS: case T_FORM: case T_CAPTION: case T_HEADER:
    case T_FOOTER: case T_SECTION: case T_ARTICLE: case T_ASIDE: case T_NAV: case T_MAIN: case T_ADDRESS:
    case T_HGROUP: case T_DIALOG:
        return HG_SMALL;
    case T_DIV: case T_LI: case T_DT: case T_DD: case T_TR: case T_CENTER: case T_FIGCAPTION: case T_LEGEND:
    case T_SUMMARY: case T_BODY: case T_HTML: case T_NOSCRIPT: case T_THEAD: case T_TBODY: case T_TFOOT:
    case T_OPTGROUP: case T_IFRAME:
        return HG_LINE;
    }
    return -1;
}

/* ----------------------------------------------------------------- state */

typedef struct {
    uint8_t flags, head, indent;
    int link;
    int skip;       /* > 0: inside something not shown */
    int form;
    int list;       /* stack index of the innermost ul/ol, -1 */
} Style;

typedef struct {
    int tag;
    Style before;   /* restored when this element closes */
    int counter;    /* ol/ul: items so far */
    int ordered;
    int row_cells;  /* tr: cells so far */
} Elem;

typedef struct {
    HtmlDoc *d;
    int cap_items, cap_text, cap_strs, cap_links, cap_imgs, cap_ctrls, cap_opts, cap_forms, cap_anchors;
    Elem stack[MAX_DEPTH];
    int depth;
    Style st;
    int run;          /* index of the open HI_TEXT item, -1 */
    int at_start;     /* at the start of a line: leading white space is dropped */
    int space;        /* a collapsed space is owed before the next character */
    int oom;
    /* text captured for a control or the title instead of being shown */
    char cap[1024];
    int cap_len, capturing;
    int cap_ctrl;     /* the <button> / <select> being filled, -1 */
    int cap_opt;      /* the <option> being filled, -1 */
    int cur_select;   /* the open <select>, -1 */
    int in_title;
} P;

static void *grow(void *p, int *cap, int need, int size, P *ps)
{
    if (need <= *cap)
        return p;
    int nc = *cap ? *cap * 2 : 64;
    while (nc < need) nc *= 2;
    void *q = realloc(p, (size_t)nc * size);
    if (!q) {
        ps->oom = 1;
        return NULL;
    }
    *cap = nc;
    return q;
}

#define GROW(ps, arr, cnt, capf) \
    ((ps)->d->arr = grow((ps)->d->arr, &(ps)->capf, (ps)->d->cnt + 1, sizeof(*(ps)->d->arr), (ps)), (ps)->d->arr != NULL)

static int str_add(P *p, const char *s, int len)
{
    if (len < 0) len = (int)strlen(s);
    HtmlDoc *d = p->d;
    if (d->strs_len + len + 1 > MAX_POOL) {
        d->truncated = 1;
        return 0; /* offset 0 is always "" */
    }
    char *n = grow(d->strs, &p->cap_strs, d->strs_len + len + 1, 1, p);
    if (!n) return 0;
    d->strs = n;
    memcpy(d->strs + d->strs_len, s, len);
    d->strs[d->strs_len + len] = 0;
    int off = d->strs_len;
    d->strs_len += len + 1;
    return off;
}

static HtmlItem *item_add(P *p, int kind)
{
    HtmlDoc *d = p->d;
    if (d->nitems >= MAX_ITEMS) {
        d->truncated = 1;
        return NULL;
    }
    HtmlItem *n = grow(d->items, &p->cap_items, d->nitems + 1, sizeof(HtmlItem), p);
    if (!n) return NULL;
    d->items = n;
    HtmlItem *it = &d->items[d->nitems++];
    it->kind = (uint8_t)kind;
    it->flags = p->st.flags;
    it->head = p->st.head;
    it->indent = p->st.indent;
    it->link = p->st.link;
    it->a = it->b = 0;
    return it;
}

static int text_put(P *p, const char *s, int len)
{
    HtmlDoc *d = p->d;
    if (d->text_len + len + 1 > MAX_POOL) {
        d->truncated = 1;
        return -1;
    }
    char *n = grow(d->text, &p->cap_text, d->text_len + len + 1, 1, p);
    if (!n) return -1;
    d->text = n;
    memcpy(d->text + d->text_len, s, len);
    d->text_len += len;
    d->text[d->text_len] = 0;
    return 0;
}

static void end_run(P *p) { p->run = -1; }

/* Appends shown text (already decoded and folded) to the current run. */
static void emit(P *p, const char *s, int len)
{
    if (p->st.skip || len <= 0)
        return;
    HtmlDoc *d = p->d;
    HtmlItem *r = p->run >= 0 ? &d->items[p->run] : NULL;
    if (!r || r->flags != p->st.flags || r->link != p->st.link || r->head != p->st.head || r->indent != p->st.indent ||
        r->a + r->b != d->text_len) {
        r = item_add(p, HI_TEXT);
        if (!r) return;
        r->a = d->text_len;
        r->b = 0;
        p->run = d->nitems - 1;
    }
    if (text_put(p, s, len) == 0)
        d->items[p->run].b += len;
}

static void owe_space(P *p)
{
    if (!p->at_start) p->space = 1;
}

/* One shown character (or several bytes from fold()). */
static void put_char(P *p, unsigned cp)
{
    if (p->capturing) { /* button labels, option texts, the title */
        if (cp == ' ' || cp == '\n' || cp == '\t' || cp == '\r' || cp == '\f') {
            if (p->cap_len > 0 && p->cap[p->cap_len - 1] != ' ' && p->cap_len < (int)sizeof(p->cap) - 1)
                p->cap[p->cap_len++] = ' ';
        } else if (p->cap_len < (int)sizeof(p->cap) - 5) {
            const char *f = fold(cp);
            if (f) {
                int l = (int)strlen(f);
                memcpy(p->cap + p->cap_len, f, l);
                p->cap_len += l;
            } else {
                p->cap_len += put_utf8(cp == 0xA0 ? ' ' : cp, p->cap + p->cap_len);
            }
        }
        return;
    }
    if (p->st.skip)
        return;
    if (p->st.flags & HF_PRE) {
        if (cp == '\r') return;
        if (cp == '\n') {
            end_run(p);
            item_add(p, HI_BREAK);
            p->at_start = 1;
            return;
        }
        if (cp == '\t') { emit(p, "    ", 4); p->at_start = 0; return; }
    } else if (cp == ' ' || cp == '\n' || cp == '\t' || cp == '\r' || cp == '\f') {
        owe_space(p);
        return;
    }
    char b[8];
    int n;
    const char *f = fold(cp);
    if (f) {
        n = (int)strlen(f);
        if (!n) return;
        if (n == 1 && f[0] == ' ' && !(p->st.flags & HF_PRE)) { owe_space(p); return; }
        memcpy(b, f, n);
    } else {
        n = put_utf8(cp == 0xA0 ? ' ' : cp, b);
    }
    if (p->space) {
        emit(p, " ", 1);
        p->space = 0;
    }
    emit(p, b, n);
    p->at_start = 0;
}

static void text_bytes(P *p, const char *s, const char *end)
{
    const unsigned char *u = (const unsigned char *)s, *e = (const unsigned char *)end;
    while (u < e) {
        if (*u == '&') {
            int used;
            unsigned cp = entity((const char *)u + 1, end, &used);
            if (used) {
                put_char(p, cp);
                u += 1 + used;
                continue;
            }
            put_char(p, '&');
            u++;
            continue;
        }
        if (*u < 0x80) {
            if (*u >= 0x20 || *u == '\n' || *u == '\t' || *u == '\r')
                put_char(p, *u);
            u++;
        } else {
            put_char(p, get_utf8(&u, e));
        }
    }
}

/* A block boundary. */
static void para(P *p, int gap)
{
    if (p->st.skip || p->capturing)
        return;
    end_run(p);
    p->space = 0;
    p->at_start = 1;
    HtmlDoc *d = p->d;
    if (d->nitems == 0)
        return;
    HtmlItem *last = &d->items[d->nitems - 1];
    if (last->kind == HI_CELL || last->kind == HI_BULLET)
        return; /* a cell or list item that starts with a block: keep it on the row / by its marker */
    if (last->kind == HI_PARA) {
        if (gap > last->a) last->a = gap;
        last->indent = p->st.indent;
        last->flags = p->st.flags;
        return;
    }
    HtmlItem *it = item_add(p, HI_PARA);
    if (it) it->a = gap;
}

/* ---------------------------------------------------------------- attrs */

typedef struct { char name[24]; char *val; int vlen; } Attr;
typedef struct {
    Attr a[MAX_ATTRS];
    int n;
    char buf[8192]; /* decoded values */
    int used;
} Attrs;

static const char *attr(const Attrs *as, const char *name)
{
    for (int i = 0; i < as->n; i++)
        if (!strcmp(as->a[i].name, name))
            return as->a[i].val;
    return NULL;
}

static int attr_int(const Attrs *as, const char *name)
{
    const char *v = attr(as, name);
    if (!v) return 0;
    while (*v == ' ') v++;
    int n = 0;
    while (isdigit((unsigned char)*v)) { n = n * 10 + (*v - '0'); v++; if (n > 100000) break; }
    if (*v == '%') return 0; /* relative sizes mean nothing here */
    return n;
}

/* Parses attributes from s up to the tag's '>' (or end). Returns where the tag ends (after '>'),
 * and sets *self_close for "/>". */
static const char *parse_attrs(const char *s, const char *end, Attrs *as, int *self_close)
{
    as->n = 0;
    as->used = 0;
    *self_close = 0;
    while (s < end) {
        while (s < end && (isspace((unsigned char)*s) || *s == '/')) {
            if (*s == '/' && s + 1 < end && s[1] == '>') *self_close = 1;
            s++;
        }
        if (s >= end) break;
        if (*s == '>') return s + 1;
        const char *ns = s;
        while (s < end && !isspace((unsigned char)*s) && *s != '=' && *s != '>' && !(*s == '/' && s + 1 < end && s[1] == '>'))
            s++;
        int nl = (int)(s - ns);
        while (s < end && isspace((unsigned char)*s)) s++;
        const char *vs = NULL;
        int vl = 0;
        if (s < end && *s == '=') {
            s++;
            while (s < end && isspace((unsigned char)*s)) s++;
            if (s < end && (*s == '"' || *s == '\'')) {
                char q = *s++;
                vs = s;
                while (s < end && *s != q) s++;
                vl = (int)(s - vs);
                if (s < end) s++;
            } else {
                vs = s;
                while (s < end && !isspace((unsigned char)*s) && *s != '>') s++;
                vl = (int)(s - vs);
            }
        }
        if (as->n < MAX_ATTRS && nl > 0 && nl < (int)sizeof(as->a[0].name)) {
            Attr *a = &as->a[as->n];
            for (int i = 0; i < nl; i++) a->name[i] = (char)tolower((unsigned char)ns[i]);
            a->name[nl] = 0;
            int room = (int)sizeof(as->buf) - as->used;
            if (room > 1) {
                char *o = as->buf + as->used;
                if (vs) html_decode_entities(vs, vl, o, room);
                else o[0] = 0;
                a->val = o;
                a->vlen = (int)strlen(o);
                as->used += a->vlen + 1;
                as->n++;
            }
        }
    }
    return s;
}

static int hidden_by_attrs(const Attrs *as)
{
    if (attr(as, "hidden")) return 1;
    const char *st = attr(as, "style");
    if (st) {
        char c[256];
        int n = 0;
        for (const char *s = st; *s && n < 255; s++)
            if (!isspace((unsigned char)*s)) c[n++] = (char)tolower((unsigned char)*s);
        c[n] = 0;
        if (strstr(c, "display:none") || strstr(c, "visibility:hidden")) return 1;
    }
    const char *ah = attr(as, "aria-hidden");
    if (ah && !strcmp(ah, "true")) {
        /* icon wrappers mostly; but keep it if it holds a link's only text. Simple rule: hide. */
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- stack */

static void pop_to(P *p, int idx)
{
    while (p->depth > idx) {
        Elem *e = &p->stack[--p->depth];
        int g = block_gap(e->tag);
        p->st = e->before;
        if (g >= 0) {
            int after = g;
            if (e->tag >= T_H1 && e->tag <= T_H6) after = HG_SMALL;
            para(p, after);
        }
    }
}

/* Index of the innermost open `tag`, searching down until one of the stop tags; -1 if none. */
static int find_open(P *p, int tag, const int *stop)
{
    for (int i = p->depth - 1; i >= 0; i--) {
        if (p->stack[i].tag == tag) return i;
        for (const int *s = stop; s && *s; s++)
            if (p->stack[i].tag == *s) return -1;
    }
    return -1;
}

static void close_open(P *p, int tag, const int *stop)
{
    int i = find_open(p, tag, stop);
    if (i >= 0) pop_to(p, i);
}

static Elem *push(P *p, int tag)
{
    if (p->depth >= MAX_DEPTH)
        return NULL;
    Elem *e = &p->stack[p->depth++];
    e->tag = tag;
    e->before = p->st;
    e->counter = 0;
    e->ordered = 0;
    e->row_cells = 0;
    return e;
}

/* --------------------------------------------------------------- forms */

static int ctrl_add(P *p, int type)
{
    HtmlDoc *d = p->d;
    if (d->nctrls >= MAX_CTRLS || !GROW(p, ctrls, nctrls, cap_ctrls))
        return -1;
    HtmlCtrl *c = &d->ctrls[d->nctrls];
    memset(c, 0, sizeof(*c));
    c->type = type;
    c->form = p->st.form;
    c->sel = -1;
    return d->nctrls++;
}

static void show_ctrl(P *p, int idx)
{
    if (idx < 0 || p->st.skip) return;
    end_run(p);
    if (p->space) { emit(p, " ", 1); p->space = 0; end_run(p); }
    HtmlItem *it = item_add(p, HI_CTRL);
    if (it) it->a = idx;
    p->at_start = 0;
    end_run(p);
}

static void start_capture(P *p)
{
    p->capturing = 1;
    p->cap_len = 0;
}

static int end_capture(P *p)
{
    p->capturing = 0;
    while (p->cap_len > 0 && p->cap[p->cap_len - 1] == ' ') p->cap_len--;
    int s = 0;
    while (s < p->cap_len && p->cap[s] == ' ') s++;
    return str_add(p, p->cap + s, p->cap_len - s);
}

static void finish_option(P *p)
{
    if (p->cap_opt < 0) return;
    HtmlDoc *d = p->d;
    HtmlOpt *o = &d->opts[p->cap_opt];
    int lab = end_capture(p);
    o->label = lab;
    if (o->value < 0) o->value = lab;
    p->cap_opt = -1;
}

static void finish_select(P *p)
{
    if (p->cur_select < 0) return;
    finish_option(p);
    HtmlCtrl *c = &p->d->ctrls[p->cur_select];
    if (c->sel < 0 && c->opt_n > 0) c->sel = 0;
    show_ctrl(p, p->cur_select);
    p->cur_select = -1;
}

static void finish_button(P *p)
{
    if (p->cap_ctrl < 0) {
        if (p->capturing && !p->in_title && p->cap_opt < 0) p->capturing = 0;
        return;
    }
    int lab = end_capture(p);
    HtmlCtrl *c = &p->d->ctrls[p->cap_ctrl];
    if (!HTML_STR(p->d, lab)[0]) lab = c->value;
    if (!HTML_STR(p->d, lab)[0]) lab = str_add(p, c->type == HC_SUBMIT ? "Submit" : "Button", -1);
    c->label = lab;
    int idx = p->cap_ctrl;
    p->cap_ctrl = -1;
    show_ctrl(p, idx);
}

/* --------------------------------------------------------------- start */

static void anchor_for(P *p, const char *name)
{
    HtmlDoc *d = p->d;
    if (!name || !name[0] || d->nanchors >= MAX_ANCHORS || !GROW(p, anchors, nanchors, cap_anchors))
        return;
    end_run(p);
    HtmlItem *it = item_add(p, HI_ANCHOR);
    if (!it) return;
    it->a = d->nanchors;
    d->anchors[d->nanchors].name = str_add(p, name, -1);
    d->anchors[d->nanchors].item = d->nitems - 1;
    d->nanchors++;
}

static void bullet(P *p, const char *s)
{
    if (p->st.skip) return;
    end_run(p);
    HtmlItem *it = item_add(p, HI_BULLET);
    if (!it) return;
    it->a = p->d->text_len;
    it->b = (int)strlen(s);
    text_put(p, s, it->b);
    p->at_start = 1;
    p->space = 0;
}

static void image(P *p, const Attrs *as, int as_ctrl)
{
    if (p->st.skip) return;
    HtmlDoc *d = p->d;
    int w = attr_int(as, "width"), h = attr_int(as, "height");
    if ((attr(as, "width") && w > 0 && w <= 2) || (attr(as, "height") && h > 0 && h <= 2))
        return; /* a tracking pixel */
    const char *src = attr(as, "src");
    const char *lazy = attr(as, "data-src");
    if (!lazy) lazy = attr(as, "data-original");
    if (!lazy) lazy = attr(as, "data-lazy-src");
    if (lazy && lazy[0] && (!src || !src[0] || !strncmp(src, "data:", 5))) src = lazy;
    if (!src || !src[0]) {
        const char *set = attr(as, "srcset");
        if (!set) set = attr(as, "data-srcset");
        if (set) {
            static char first[1024];
            int n = 0;
            while (*set == ' ') set++;
            while (set[n] && set[n] != ' ' && set[n] != ',' && n < 1023) { first[n] = set[n]; n++; }
            first[n] = 0;
            src = first;
        }
    }
    const char *alt = attr(as, "alt");
    if (!src) src = "";
    if (!GROW(p, imgs, nimgs, cap_imgs)) return;
    HtmlImg *im = &d->imgs[d->nimgs];
    im->src = str_add(p, src, -1);
    im->alt = str_add(p, alt ? alt : "", -1);
    im->w = w;
    im->h = h;
    im->link = p->st.link;
    int idx = d->nimgs++;
    if (as_ctrl >= 0) { /* <input type=image> */
        d->ctrls[as_ctrl].value = im->src;
        d->ctrls[as_ctrl].label = HTML_STR(d, im->alt)[0] ? im->alt : str_add(p, "Submit", -1);
        show_ctrl(p, as_ctrl);
        return;
    }
    end_run(p);
    if (p->space) { emit(p, " ", 1); p->space = 0; end_run(p); }
    HtmlItem *it = item_add(p, HI_IMG);
    if (it) it->a = idx;
    p->at_start = 0;
}

static void input(P *p, const Attrs *as)
{
    if (p->st.skip) return;
    const char *t = attr(as, "type");
    char type[16] = "text";
    if (t) {
        int i = 0;
        for (; t[i] && i < 15; i++) type[i] = (char)tolower((unsigned char)t[i]);
        type[i] = 0;
    }
    int k;
    if (!strcmp(type, "hidden")) k = HC_HIDDEN;
    else if (!strcmp(type, "password")) k = HC_PASSWORD;
    else if (!strcmp(type, "submit")) k = HC_SUBMIT;
    else if (!strcmp(type, "image")) k = HC_IMAGE;
    else if (!strcmp(type, "button")) k = HC_BUTTON;
    else if (!strcmp(type, "checkbox")) k = HC_CHECKBOX;
    else if (!strcmp(type, "radio")) k = HC_RADIO;
    else if (!strcmp(type, "reset") || !strcmp(type, "file") || !strcmp(type, "range") || !strcmp(type, "color")) return;
    else k = HC_TEXT;
    if ((k == HC_CHECKBOX || k == HC_RADIO) && !attr(as, "name") && p->st.form < 0)
        return; /* a style sheet's menu toggle, not a form field */
    int idx = ctrl_add(p, k);
    if (idx < 0) return;
    HtmlCtrl *c = &p->d->ctrls[idx];
    const char *nm = attr(as, "name"), *val = attr(as, "value"), *ph = attr(as, "placeholder");
    c->name = str_add(p, nm ? nm : "", -1);
    c = &p->d->ctrls[idx]; /* str_add never moves ctrls, but keep it obvious */
    if (k == HC_CHECKBOX || k == HC_RADIO)
        c->value = str_add(p, val ? val : "on", -1);
    else
        c->value = str_add(p, val ? val : "", -1);
    c->checked = attr(as, "checked") != NULL;
    c->size = attr_int(as, "size");
    if (k == HC_SUBMIT || k == HC_BUTTON)
        c->label = str_add(p, val && val[0] ? val : (k == HC_SUBMIT ? "Submit" : "Button"), -1);
    else
        c->label = str_add(p, ph ? ph : (attr(as, "aria-label") ? attr(as, "aria-label") : ""), -1);
    if (k == HC_HIDDEN) return;
    if (k == HC_IMAGE) { image(p, as, idx); return; }
    show_ctrl(p, idx);
}

static const int s_stop_list[] = {T_UL, T_OL, T_MENU, T_TABLE, 0};
static const int s_stop_dl[] = {T_DL, T_TABLE, 0};
static const int s_stop_row[] = {T_TABLE, 0};
static const int s_stop_cell[] = {T_TR, T_TABLE, 0};
static const int s_stop_p[] = {T_BUTTON, T_TD, T_TH, T_LI, T_BLOCKQUOTE, T_TABLE, T_DD, T_FIGURE, T_DETAILS, 0};
static const int s_stop_a[] = {T_TD, T_TH, T_TABLE, 0};

static void start_tag(P *p, int t, const char *name, const Attrs *as, int self_close)
{
    HtmlDoc *d = p->d;
    /* implied ends */
    int g = block_gap(t);
    if (g >= 0 || t == T_TABLE || t == T_HR)
        close_open(p, T_P, s_stop_p);
    switch (t) {
    case T_LI: close_open(p, T_LI, s_stop_list); break;
    case T_DT: case T_DD: close_open(p, T_DT, s_stop_dl); close_open(p, T_DD, s_stop_dl); break;
    case T_TR: close_open(p, T_TR, s_stop_row); break;
    case T_TD: case T_TH: close_open(p, T_TD, s_stop_cell); close_open(p, T_TH, s_stop_cell); break;
    case T_A: close_open(p, T_A, s_stop_a); break;
    case T_BODY: close_open(p, T_HEAD, NULL); break;
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
        for (int h = T_H1; h <= T_H6; h++) close_open(p, h, s_stop_p);
        break;
    case T_OPTION: case T_OPTGROUP:
        finish_option(p);
        break;
    }

    const char *id = attr(as, "id");
    if (!id && t == T_A) id = attr(as, "name");
    if (id && !p->st.skip)
        anchor_for(p, id);

    /* elements with no content */
    switch (t) {
    case T_BR:
        if (!p->st.skip && !p->capturing) {
            end_run(p);
            item_add(p, HI_BREAK);
            p->at_start = 1;
            p->space = 0;
        }
        return;
    case T_WBR:
        return;
    case T_HR:
        if (!p->st.skip) {
            para(p, HG_SMALL);
            item_add(p, HI_RULE);
            para(p, HG_SMALL);
        }
        return;
    case T_IMG:
        if (!p->capturing) image(p, as, -1);
        else if (attr(as, "alt")) { /* an image in a button: its alt is the label */
            const char *al = attr(as, "alt");
            int l = (int)strlen(al);
            if (p->cap_len + l < (int)sizeof(p->cap) - 1) { memcpy(p->cap + p->cap_len, al, l); p->cap_len += l; }
        }
        return;
    case T_INPUT:
        if (!p->capturing) input(p, as);
        return;
    case T_BASE: {
        const char *h = attr(as, "href");
        if (h && d->base < 0) d->base = str_add(p, h, -1);
        return;
    }
    case T_META: {
        const char *he = attr(as, "http-equiv"), *c = attr(as, "content");
        if (he && c && !strcasecmp(he, "refresh") && d->refresh < 0) {
            int sec = atoi(c);
            const char *u = strcasestr(c, "url=");
            if (u) {
                u += 4;
                while (*u == ' ' || *u == '\'' || *u == '"') u++;
                char tmp[1024];
                snprintf(tmp, sizeof(tmp), "%s", u);
                int l = (int)strlen(tmp);
                while (l > 0 && (tmp[l - 1] == '\'' || tmp[l - 1] == '"' || tmp[l - 1] == ' ')) tmp[--l] = 0;
                d->refresh = str_add(p, tmp, -1);
                d->refresh_sec = sec;
            }
        }
        return;
    }
    case T_LINK: case T_PARAM: case T_TRACK: case T_AREA: case T_COL: case T_EMBED: case T_FRAME:
        return;
    case T_SOURCE:
        return; /* <picture>'s alternatives: the <img> inside is used */
    }
    if (is_void(t))
        return;
    if (self_close && (t == T_UNKNOWN || t == T_SVG || t == T_MATH || is_skipped(t)))
        return; /* <svg/>, <foo/> */

    if (t == T_SELECT) {
        finish_select(p);
        int idx = ctrl_add(p, HC_SELECT);
        if (idx >= 0) {
            const char *nm = attr(as, "name");
            d->ctrls[idx].name = str_add(p, nm ? nm : "", -1);
            d->ctrls[idx].opt_first = d->nopts;
            p->cur_select = p->st.skip ? -1 : idx;
            if (p->st.skip) d->nctrls--; /* not shown: drop it */
        }
    }
    if (t == T_OPTION && p->cur_select >= 0) {
        if (d->nopts < MAX_OPTS && GROW(p, opts, nopts, cap_opts)) {
            HtmlOpt *o = &d->opts[d->nopts];
            const char *v = attr(as, "value");
            o->value = v ? str_add(p, v, -1) : -1;
            o->label = 0;
            HtmlCtrl *c = &d->ctrls[p->cur_select];
            if (attr(as, "selected")) c->sel = c->opt_n;
            c->opt_n++;
            p->cap_opt = d->nopts++;
            start_capture(p);
        }
    }

    Elem *e = push(p, t);
    if (!e) return;

    if (hidden_by_attrs(as) && t != T_HTML && t != T_BODY)
        p->st.skip++;
    if (is_skipped(t))
        p->st.skip++;
    if (t == T_DIALOG && !attr(as, "open"))
        p->st.skip++;

    if (g >= 0) {
        int before = g;
        if (t == T_DIV && attr(as, "role") && !strcmp(attr(as, "role"), "heading")) before = HG_PARA;
        para(p, before);
    }

    switch (t) {
    case T_TITLE:
        if (!d->title[0]) { p->in_title = 1; start_capture(p); }
        break;
    case T_TEXTAREA: {
        int idx = ctrl_add(p, HC_TEXTAREA);
        if (idx >= 0) {
            const char *nm = attr(as, "name"), *ph = attr(as, "placeholder");
            d->ctrls[idx].name = str_add(p, nm ? nm : "", -1);
            d->ctrls[idx].label = str_add(p, ph ? ph : "", -1);
            if (e->before.skip) d->nctrls--;
            else p->cap_ctrl = idx;
        }
        start_capture(p);
        break;
    }
    case T_BUTTON: {
        const char *ty = attr(as, "type");
        int k = ty && !strcasecmp(ty, "button") ? HC_BUTTON : ty && !strcasecmp(ty, "reset") ? -1 : HC_SUBMIT;
        int idx = k >= 0 && !e->before.skip ? ctrl_add(p, k) : -1;
        if (idx >= 0) {
            const char *nm = attr(as, "name"), *v = attr(as, "value");
            d->ctrls[idx].name = str_add(p, nm ? nm : "", -1);
            d->ctrls[idx].value = str_add(p, v ? v : "", -1);
        }
        p->cap_ctrl = idx;
        start_capture(p);
        break;
    }
    case T_A: {
        const char *h = attr(as, "href");
        if (h && !p->st.skip && GROW(p, links, nlinks, cap_links)) {
            if (p->space) { /* the space before a link is not part of it */
                emit(p, " ", 1);
                p->space = 0;
            }
            d->links[d->nlinks].href = str_add(p, h, -1);
            p->st.link = d->nlinks++;
            end_run(p);
        }
        break;
    }
    case T_B: case T_STRONG: case T_DT: case T_LEGEND: case T_SUMMARY: case T_CAPTION:
        p->st.flags |= HF_BOLD;
        break;
    case T_SMALL: case T_SUB: case T_SUP:
        p->st.flags |= HF_SMALL;
        break;
    case T_FIGCAPTION:
        p->st.flags |= HF_SMALL | HF_MUTED;
        break;
    case T_S: case T_STRIKE: case T_DEL:
        p->st.flags |= HF_STRIKE | HF_MUTED;
        break;
    case T_CODE: case T_KBD: case T_SAMP: case T_TT: case T_VAR:
        p->st.flags |= HF_CODE;
        break;
    case T_PRE: case T_XMP: case T_PLAINTEXT:
        p->st.flags |= HF_PRE | HF_CODE;
        break;
    case T_CENTER:
        p->st.flags |= HF_CENTER;
        break;
    case T_TABLE:
        p->st.flags &= ~HF_CENTER; /* as in quirks mode: cells don't inherit centring */
        break;
    case T_BLOCKQUOTE:
        p->st.flags |= HF_QUOTE;
        if (p->st.indent < 8) p->st.indent++;
        break;
    case T_DD:
        if (p->st.indent < 8) p->st.indent++;
        break;
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
        p->st.head = (uint8_t)(t - T_H1 + 1);
        p->st.flags |= HF_BOLD;
        break;
    case T_UL: case T_OL: case T_MENU:
        if (p->st.indent < 8) p->st.indent++;
        e->ordered = t == T_OL;
        e->counter = 0;
        if (t == T_OL) {
            int start = attr(as, "start") ? atoi(attr(as, "start")) : 1;
            e->counter = start - 1;
        }
        p->st.list = p->depth - 1;
        break;
    case T_LI:
        if (p->st.list >= 0 && p->st.list < p->depth) {
            Elem *l = &p->stack[p->st.list];
            l->counter++;
            char b[16];
            if (l->ordered) {
                snprintf(b, sizeof(b), "%d.", l->counter);
            } else {
                int level = 0;
                for (int i = 0; i < p->depth; i++)
                    if (p->stack[i].tag == T_UL || p->stack[i].tag == T_MENU) level++;
                snprintf(b, sizeof(b), "%s", level <= 1 ? "\xE2\x80\xA2" : level == 2 ? "\xE2\x80\x93" : "\xC2\xB7");
            }
            bullet(p, b);
        } else {
            bullet(p, "\xE2\x80\xA2");
        }
        break;
    case T_TD: case T_TH: {
        if (t == T_TH) p->st.flags |= HF_BOLD;
        int r = find_open(p, T_TR, s_stop_row);
        if (r >= 0) {
            if (p->stack[r].row_cells++ > 0 && !p->st.skip) {
                end_run(p);
                HtmlItem *it = item_add(p, HI_CELL);
                (void)it;
                p->at_start = 1;
                p->space = 0;
            }
        }
        break;
    }
    case T_FORM: {
        if (GROW(p, forms, nforms, cap_forms)) {
            const char *ac = attr(as, "action"), *m = attr(as, "method");
            d->forms[d->nforms].action = str_add(p, ac ? ac : "", -1);
            d->forms[d->nforms].post = m && !strcasecmp(m, "post");
            p->st.form = d->nforms++;
        }
        break;
    }
    case T_Q:
        put_char(p, 0x201C);
        break;
    case T_IFRAME: {
        const char *src = attr(as, "src");
        if (src && src[0] && !p->st.skip && GROW(p, links, nlinks, cap_links)) {
            d->links[d->nlinks].href = str_add(p, src, -1);
            int saved = p->st.link;
            p->st.link = d->nlinks++;
            const char *ttl = attr(as, "title");
            char lab[200];
            snprintf(lab, sizeof(lab), "[Embedded page%s%s]", ttl ? ": " : "", ttl ? ttl : "");
            text_bytes(p, lab, lab + strlen(lab));
            end_run(p);
            p->st.link = saved;
        }
        p->st.skip++; /* its fallback content */
        break;
    }
    case T_HTML: case T_BODY:
        break;
    default:
        break;
    }
    (void)name;
}

static void end_tag(P *p, int t)
{
    switch (t) {
    case T_TITLE:
        if (p->in_title) {
            int s = end_capture(p);
            snprintf(p->d->title, sizeof(p->d->title), "%s", HTML_STR(p->d, s));
            p->in_title = 0;
        }
        break;
    case T_OPTION:
        finish_option(p);
        break;
    case T_SELECT: {
        int i = find_open(p, T_SELECT, NULL);
        if (i >= 0) {
            pop_to(p, i);
            finish_select(p);
        }
        return;
    }
    case T_BUTTON: {
        int i = find_open(p, T_BUTTON, NULL);
        if (i >= 0) {
            pop_to(p, i);
            finish_button(p);
        }
        return;
    }
    case T_TEXTAREA: {
        int i = find_open(p, T_TEXTAREA, NULL);
        if (i >= 0) {
            pop_to(p, i);
            if (p->cap_ctrl >= 0) {
                /* the value keeps its line breaks; captured text collapsed them, which is fine for a box */
                int v = end_capture(p);
                p->d->ctrls[p->cap_ctrl].value = v;
                int idx = p->cap_ctrl;
                p->cap_ctrl = -1;
                show_ctrl(p, idx);
            } else {
                end_capture(p);
            }
        }
        return;
    }
    case T_Q:
        put_char(p, 0x201D);
        break;
    case T_BR: /* </br> is a <br> to browsers */
        if (!p->st.skip) {
            end_run(p);
            item_add(p, HI_BREAK);
            p->at_start = 1;
        }
        return;
    case T_P: {
        int i = find_open(p, T_P, s_stop_p);
        if (i < 0) { para(p, HG_PARA); return; } /* a stray </p> makes an empty paragraph */
        break;
    }
    }
    const int *stop = NULL;
    static const int stop_inline[] = {T_TD, T_TH, T_TABLE, T_LI, T_DIV, T_P, T_BODY, 0};
    if (block_gap(t) < 0 && t != T_A && !is_skipped(t)) stop = stop_inline;
    int i = find_open(p, t, stop);
    if (i >= 0)
        pop_to(p, i);
}

/* -------------------------------------------------------------- the pass */

/* For <script>, <style>, <textarea>, <title>: where "</name" starts. */
static const char *find_close(const char *s, const char *end, const char *name)
{
    int nl = (int)strlen(name);
    for (; s + 2 + nl <= end; s++) {
        if (s[0] == '<' && s[1] == '/' && !strncasecmp(s + 2, name, nl)) {
            char c = s + 2 + nl < end ? s[2 + nl] : '>';
            if (c == '>' || isspace((unsigned char)c) || c == '/')
                return s;
        }
    }
    return end;
}

HtmlDoc *html_parse(const char *src, int len)
{
    P *p = calloc(1, sizeof(P));
    HtmlDoc *d = calloc(1, sizeof(HtmlDoc));
    if (!p || !d) {
        free(p);
        free(d);
        return NULL;
    }
    p->d = d;
    d->base = d->refresh = -1;
    p->run = -1;
    p->at_start = 1;
    p->st.link = -1;
    p->st.form = -1;
    p->st.list = -1;
    p->cap_ctrl = p->cap_opt = p->cur_select = -1;
    str_add(p, "", 0); /* offset 0: the empty string */
    static Attrs as; /* big; the parser is only ever used by one thread at a time */

    const char *s = src, *end = src + len;
    if (len >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        s += 3;
    while (s < end && !p->oom) {
        const char *lt = memchr(s, '<', end - s);
        if (!lt) lt = end;
        if (lt > s)
            text_bytes(p, s, lt);
        s = lt;
        if (s >= end)
            break;
        /* s points at '<' */
        if (s + 4 <= end && !strncmp(s, "<!--", 4)) {
            const char *c = s + 4;
            while (c + 3 <= end && strncmp(c, "-->", 3)) c++;
            s = c + 3 <= end ? c + 3 : end;
            continue;
        }
        if (s + 1 < end && (s[1] == '!' || s[1] == '?')) {
            const char *c = memchr(s, '>', end - s);
            s = c ? c + 1 : end;
            continue;
        }
        int closing = s + 1 < end && s[1] == '/';
        const char *ns = s + 1 + closing;
        if (ns >= end || !isalpha((unsigned char)*ns)) {
            text_bytes(p, s, s + 1); /* a lone '<' */
            s++;
            continue;
        }
        char name[16];
        int nl = 0;
        const char *q = ns;
        while (q < end && (isalnum((unsigned char)*q) || *q == '-' || *q == ':' || *q == '_')) {
            if (nl < 15) name[nl++] = (char)tolower((unsigned char)*q);
            q++;
        }
        name[nl] = 0;
        int t = q - ns <= 15 ? tag_id(name) : T_UNKNOWN;
        int self_close;
        const char *after = parse_attrs(q, end, &as, &self_close);
        if (closing) {
            end_tag(p, t);
        } else {
            start_tag(p, t, name, &as, self_close);
            /* raw text: the content up to the matching end tag, untouched */
            if (t == T_SCRIPT || t == T_STYLE || t == T_TEXTAREA || t == T_TITLE || t == T_XMP ||
                (t == T_NOSCRIPT && 0)) {
                const char *c = find_close(after, end, name);
                if (t == T_TEXTAREA || t == T_TITLE)
                    text_bytes(p, after, c);
                after = c;
            } else if (t == T_PLAINTEXT) {
                text_bytes(p, after, end);
                after = end;
            }
        }
        s = after;
    }
    finish_select(p);
    finish_button(p);
    if (p->in_title) {
        int s2 = end_capture(p);
        snprintf(d->title, sizeof(d->title), "%s", HTML_STR(d, s2));
    }
    pop_to(p, 0);
    int oom = p->oom;
    free(p);
    if (oom) {
        html_free(d);
        return NULL;
    }
    /* drop a trailing block boundary */
    while (d->nitems > 0 && d->items[d->nitems - 1].kind == HI_PARA)
        d->nitems--;
    return d;
}

void html_free(HtmlDoc *d)
{
    if (!d) return;
    free(d->items);
    free(d->text);
    free(d->strs);
    free(d->links);
    free(d->imgs);
    free(d->ctrls);
    free(d->opts);
    free(d->forms);
    free(d->anchors);
    free(d);
}

void html_sniff_charset(const char *b, int n, char *out, int on)
{
    out[0] = 0;
    if (n > 4096) n = 4096;
    for (int i = 0; i + 8 < n; i++) {
        if (strncasecmp(b + i, "charset", 7)) continue;
        int j = i + 7;
        while (j < n && (b[j] == ' ' || b[j] == '\t')) j++;
        if (j >= n || b[j] != '=') continue;
        j++;
        while (j < n && (b[j] == ' ' || b[j] == '"' || b[j] == '\'')) j++;
        int k = 0;
        while (j < n && k < on - 1 && (isalnum((unsigned char)b[j]) || b[j] == '-' || b[j] == '_'))
            out[k++] = (char)tolower((unsigned char)b[j++]);
        out[k] = 0;
        if (k) return;
    }
}
