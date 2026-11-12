/* XLT - URL helpers. See url.h. */
#define _GNU_SOURCE /* strcasestr */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "url.h"

/* Splits url into its five parts (pointers into it, with lengths). A part
 * that is absent has len -1 (an empty but present part, "?" alone, has 0). */
typedef struct { const char *p; int len; } Part;
typedef struct { Part scheme, auth, path, query, frag; } Parts;

static void split(const char *u, Parts *o)
{
    o->scheme.len = o->auth.len = o->query.len = o->frag.len = -1;
    o->scheme.p = o->auth.p = o->query.p = o->frag.p = u;
    const char *s = u;
    /* scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":" */
    if (isalpha((unsigned char)*s)) {
        const char *t = s;
        while (isalnum((unsigned char)*t) || *t == '+' || *t == '-' || *t == '.')
            t++;
        if (*t == ':') {
            o->scheme.p = s;
            o->scheme.len = (int)(t - s);
            s = t + 1;
        }
    }
    if (s[0] == '/' && s[1] == '/') {
        s += 2;
        const char *t = s;
        while (*t && *t != '/' && *t != '?' && *t != '#')
            t++;
        o->auth.p = s;
        o->auth.len = (int)(t - s);
        s = t;
    }
    const char *t = s;
    while (*t && *t != '?' && *t != '#')
        t++;
    o->path.p = s;
    o->path.len = (int)(t - s);
    s = t;
    if (*s == '?') {
        t = ++s;
        while (*t && *t != '#')
            t++;
        o->query.p = s;
        o->query.len = (int)(t - s);
        s = t;
    }
    if (*s == '#') {
        o->frag.p = s + 1;
        o->frag.len = (int)strlen(s + 1);
    }
}

typedef struct { char *b; int n, len, over; } Buf;

static void put(Buf *b, const char *s, int len)
{
    if (len < 0) len = (int)strlen(s);
    if (b->len + len >= b->n) {
        b->over = 1;
        len = b->n - 1 - b->len;
        if (len < 0) len = 0;
    }
    memcpy(b->b + b->len, s, len);
    b->len += len;
    b->b[b->len] = 0;
}

/* RFC 3986 5.2.4, on the path in p (len bytes), appended to b. */
static void remove_dots(Buf *b, const char *p, int len)
{
    char in[URL_MAX];
    if (len >= URL_MAX) len = URL_MAX - 1;
    memcpy(in, p, len);
    in[len] = 0;
    char out[URL_MAX];
    int ol = 0;
    char *s = in;
    while (*s) {
        if (!strncmp(s, "../", 3)) s += 3;
        else if (!strncmp(s, "./", 2)) s += 2;
        else if (!strncmp(s, "/./", 3)) s += 2;
        else if (!strcmp(s, "/.")) { s[1] = 0; }
        else if (!strncmp(s, "/../", 4) || !strcmp(s, "/..")) {
            if (s[3] == '/') s += 3;
            else { s += 2; s[0] = '/'; }
            while (ol > 0 && out[ol - 1] != '/') ol--;
            if (ol > 0) ol--; /* the '/' before the dropped segment */
        } else if (!strcmp(s, ".") || !strcmp(s, "..")) {
            break;
        } else {
            int k = 0;
            if (s[0] == '/') k = 1;
            while (s[k] && s[k] != '/') k++;
            if (ol + k < URL_MAX) {
                memcpy(out + ol, s, k);
                ol += k;
            }
            s += k;
        }
    }
    put(b, out, ol);
}

int url_resolve(const char *base, const char *ref, char *out, int n)
{
    if (n <= 0)
        return -1;
    out[0] = 0;
    while (*ref == ' ' || *ref == '\t' || *ref == '\n' || *ref == '\r')
        ref++;
    char r[URL_MAX];
    int rl = 0;
    for (const char *s = ref; *s && rl < URL_MAX - 1; s++) /* drop tabs/newlines inside, trim the end */
        if (*s != '\t' && *s != '\n' && *s != '\r')
            r[rl++] = *s;
    while (rl > 0 && r[rl - 1] == ' ')
        rl--;
    r[rl] = 0;

    Parts B, R;
    split(base, &B);
    split(r, &R);
    Buf b = {out, n, 0, 0};
    if (R.scheme.len > 0) {
        char sc[16];
        int l = R.scheme.len < 15 ? R.scheme.len : 15;
        for (int i = 0; i < l; i++) sc[i] = (char)tolower((unsigned char)R.scheme.p[i]);
        sc[l] = 0;
        put(&b, sc, -1);
        put(&b, ":", 1);
        if (R.auth.len >= 0) { put(&b, "//", 2); put(&b, R.auth.p, R.auth.len); }
        remove_dots(&b, R.path.p, R.path.len);
        if (R.auth.len >= 0 && R.path.len == 0) put(&b, "/", 1);
        if (R.query.len >= 0) { put(&b, "?", 1); put(&b, R.query.p, R.query.len); }
    } else {
        if (B.scheme.len <= 0)
            return -1;
        put(&b, B.scheme.p, B.scheme.len);
        put(&b, ":", 1);
        if (R.auth.len >= 0) {
            put(&b, "//", 2);
            put(&b, R.auth.p, R.auth.len);
            remove_dots(&b, R.path.p, R.path.len);
            if (R.path.len == 0) put(&b, "/", 1);
            if (R.query.len >= 0) { put(&b, "?", 1); put(&b, R.query.p, R.query.len); }
        } else {
            if (B.auth.len >= 0) { put(&b, "//", 2); put(&b, B.auth.p, B.auth.len); }
            if (R.path.len == 0) {
                if (B.path.len == 0 && B.auth.len >= 0) put(&b, "/", 1);
                else put(&b, B.path.p, B.path.len);
                if (R.query.len >= 0) { put(&b, "?", 1); put(&b, R.query.p, R.query.len); }
                else if (B.query.len >= 0) { put(&b, "?", 1); put(&b, B.query.p, B.query.len); }
            } else {
                char merged[URL_MAX];
                int ml = 0;
                if (R.path.p[0] == '/') {
                    ml = snprintf(merged, sizeof(merged), "%.*s", R.path.len, R.path.p);
                } else {
                    int keep = 0;
                    if (B.auth.len >= 0 && B.path.len == 0) {
                        merged[0] = '/';
                        ml = 1;
                    } else {
                        for (int i = 0; i < B.path.len; i++)
                            if (B.path.p[i] == '/') keep = i + 1;
                        ml = snprintf(merged, sizeof(merged), "%.*s", keep, B.path.p);
                    }
                    if (ml < (int)sizeof(merged))
                        ml += snprintf(merged + ml, sizeof(merged) - ml, "%.*s", R.path.len, R.path.p);
                }
                if (ml >= (int)sizeof(merged))
                    return -1;
                remove_dots(&b, merged, ml);
                if (R.query.len >= 0) { put(&b, "?", 1); put(&b, R.query.p, R.query.len); }
            }
        }
    }
    if (R.frag.len >= 0) { put(&b, "#", 1); put(&b, R.frag.p, R.frag.len); }
    if (b.over)
        return -1;
    /* spaces are not allowed in a URL: send them encoded */
    if (strchr(out, ' ')) {
        char tmp[URL_MAX];
        int tl = 0;
        for (const char *s = out; *s; s++) {
            if (*s == ' ') {
                if (tl + 3 >= URL_MAX) return -1;
                memcpy(tmp + tl, "%20", 3);
                tl += 3;
            } else {
                if (tl + 1 >= URL_MAX) return -1;
                tmp[tl++] = *s;
            }
        }
        tmp[tl] = 0;
        if (tl >= n) return -1;
        memcpy(out, tmp, tl + 1);
    }
    return 0;
}

void url_scheme(const char *url, char *out, int n)
{
    Parts P;
    split(url, &P);
    int l = P.scheme.len > 0 ? P.scheme.len : 0;
    if (l >= n) l = n - 1;
    for (int i = 0; i < l; i++) out[i] = (char)tolower((unsigned char)P.scheme.p[i]);
    out[l] = 0;
}

void url_host(const char *url, char *out, int n)
{
    Parts P;
    split(url, &P);
    out[0] = 0;
    if (P.auth.len <= 0)
        return;
    const char *a = P.auth.p, *e = a + P.auth.len;
    for (const char *t = a; t < e; t++)
        if (*t == '@') a = t + 1;
    const char *c = a;
    if (*c == '[') { /* IPv6 literal */
        while (c < e && *c != ']') c++;
        if (c < e) c++;
    } else {
        while (c < e && *c != ':') c++;
    }
    int l = (int)(c - a);
    if (l >= n) l = n - 1;
    for (int i = 0; i < l; i++) out[i] = (char)tolower((unsigned char)a[i]);
    out[l] = 0;
}

int url_is_web(const char *url)
{
    char sc[16];
    url_scheme(url, sc, sizeof(sc));
    char h[8];
    url_host(url, h, sizeof(h));
    return (!strcmp(sc, "http") || !strcmp(sc, "https")) && h[0];
}

int url_same_page(const char *a, const char *b)
{
    const char *ha = strchr(a, '#'), *hb = strchr(b, '#');
    int la = ha ? (int)(ha - a) : (int)strlen(a), lb = hb ? (int)(hb - b) : (int)strlen(b);
    return la == lb && !strncmp(a, b, la);
}

const char *url_fragment(const char *url)
{
    const char *h = strchr(url, '#');
    return h ? h + 1 : "";
}

int url_encode_append(char *out, int n, const char *s)
{
    int l = (int)strlen(out);
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char t[4];
        int k;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '*') { t[0] = (char)c; k = 1; }
        else if (c == ' ') { t[0] = '+'; k = 1; }
        else if (c == '\n') { memcpy(t, "%0D", 3); k = 3; /* newlines go as CR LF */
            if (l + 3 >= n) return -1;
            memcpy(out + l, t, 3); l += 3; memcpy(t, "%0A", 3); }
        else { t[0] = '%'; t[1] = hex[c >> 4]; t[2] = hex[c & 15]; k = 3; }
        if (l + k >= n) {
            out[l] = 0;
            return -1;
        }
        memcpy(out + l, t, k);
        l += k;
    }
    out[l] = 0;
    return 0;
}

/* Does the typed text look like an address? */
static int looks_like_address(const char *t)
{
    if (strchr(t, ' '))
        return 0;
    char sc[16];
    url_scheme(t, sc, sizeof(sc));
    if (!strcmp(sc, "http") || !strcmp(sc, "https"))
        return 1;
    if (!strncmp(t, "localhost", 9) && (t[9] == 0 || t[9] == ':' || t[9] == '/'))
        return 1;
    /* host part: up to / ? # ; needs a dot not at either end, and a TLD of letters (or an IPv4) */
    int hl = (int)strcspn(t, "/?#");
    char host[256];
    if (hl <= 0 || hl >= (int)sizeof(host))
        return 0;
    memcpy(host, t, hl);
    host[hl] = 0;
    char *colon = strchr(host, ':');
    if (colon) {
        for (char *p = colon + 1; *p; p++)
            if (!isdigit((unsigned char)*p)) return 0;
        *colon = 0;
    }
    char *dot = strrchr(host, '.');
    if (!dot || dot == host || !dot[1])
        return 0;
    int digits = 1, letters = 1;
    for (char *p = dot + 1; *p; p++) {
        if (!isdigit((unsigned char)*p)) digits = 0;
        if (!isalpha((unsigned char)*p)) letters = 0;
    }
    for (char *p = host; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '.') return 0;
    if (letters)
        return (int)strlen(dot + 1) >= 2;
    if (!digits)
        return 0;
    int dots = 0; /* an IPv4 address: four numbers */
    for (char *p = host; *p; p++) {
        if (*p == '.') dots++;
        else if (!isdigit((unsigned char)*p)) return 0;
    }
    return dots == 3;
}

int url_from_input(const char *text, const char *search_prefix, char *out, int n)
{
    while (*text == ' ') text++;
    char t[URL_MAX];
    snprintf(t, sizeof(t), "%s", text);
    int l = (int)strlen(t);
    while (l > 0 && t[l - 1] == ' ') t[--l] = 0;
    if (!l)
        return -1;
    if (looks_like_address(t)) {
        char sc[16];
        url_scheme(t, sc, sizeof(sc));
        if (!strcmp(sc, "http") || !strcmp(sc, "https")) {
            if (url_resolve(t, "", out, n) < 0) snprintf(out, n, "%s", t);
        } else {
            char full[URL_MAX + 16];
            snprintf(full, sizeof(full), "https://%s", t);
            if (url_resolve(full, "", out, n) < 0) snprintf(out, n, "%s", full);
        }
        return 1;
    }
    snprintf(out, n, "%s", search_prefix);
    url_encode_append(out, n, t);
    return 0;
}

void url_pretty(const char *url, char *out, int n)
{
    const char *s = url;
    if (!strncmp(s, "https://", 8)) s += 8;
    else if (!strncmp(s, "http://", 7)) s += 7;
    if (!strncmp(s, "www.", 4)) s += 4;
    snprintf(out, n, "%s", s);
    int l = (int)strlen(out);
    if (l > 0 && out[l - 1] == '/' && !strchr(out, '?'))
        out[l - 1] = 0;
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int url_unwrap_redirect(const char *url, char *out, int n)
{
    char host[128];
    url_host(url, host, sizeof(host));
    int hl = (int)strlen(host);
    if (strcmp(host, "duckduckgo.com") && !(hl > 15 && !strcmp(host + hl - 15, ".duckduckgo.com")))
        return 0;
    const char *p = strstr(url, host) + hl;
    while (*p && *p != '/' && *p != '?') p++; /* a port */
    if (strncmp(p, "/l/", 3))
        return 0;
    const char *q = strchr(p, '?');
    if (!q) return 0;
    const char *v = NULL;
    for (const char *t = q + 1; *t; ) {
        if (!strncmp(t, "uddg=", 5)) { v = t + 5; break; }
        t = strchr(t, '&');
        if (!t) break;
        t++;
    }
    if (!v) return 0;
    char dec[URL_MAX];
    int k = 0;
    for (; *v && *v != '&' && *v != '#' && k < URL_MAX - 1; v++) {
        if (*v == '%' && hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
            dec[k++] = (char)(hexval(v[1]) * 16 + hexval(v[2]));
            v += 2;
        } else {
            dec[k++] = *v == '+' ? ' ' : *v;
        }
    }
    dec[k] = 0;
    if (!url_is_web(dec) || k >= n)
        return 0;
    memcpy(out, dec, k + 1);
    return 1;
}

int url_is_svg(const char *url)
{
    if (!strncasecmp(url, "data:image/svg", 14))
        return 1;
    int l = (int)strcspn(url, "?#");
    return l >= 4 && !strncasecmp(url + l - 4, ".svg", 4);
}

static void fat_clean(char *s)
{
    for (char *p = s; *p; p++)
        if (strchr("\\/:*?\"<>|", *p) || (unsigned char)*p < 0x20) *p = '_';
    /* no dots or spaces at the end, none at the start */
    int l = (int)strlen(s);
    while (l > 0 && (s[l - 1] == '.' || s[l - 1] == ' ')) s[--l] = 0;
    char *b = s;
    while (*b == ' ' || *b == '.') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

static void pct_decode(const char *s, int len, char *out, int n)
{
    int k = 0;
    for (int i = 0; i < len && k < n - 1; i++) {
        if (s[i] == '%' && i + 2 < len && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            out[k++] = (char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else {
            out[k++] = s[i];
        }
    }
    out[k] = 0;
}

void url_file_name(const char *url, const char *disposition, char *out, int n)
{
    out[0] = 0;
    if (disposition) {
        const char *f = strcasestr(disposition, "filename*=");
        if (f) { /* filename*=UTF-8''name%20here */
            f += 10;
            const char *q = strstr(f, "''");
            if (q) f = q + 2;
            int l = (int)strcspn(f, "; \r\n");
            pct_decode(f, l, out, n);
        } else if ((f = strcasestr(disposition, "filename=")) != NULL) {
            f += 9;
            int quoted = *f == '"';
            if (quoted) f++;
            int l = (int)strcspn(f, quoted ? "\"" : "; \r\n");
            if (l >= n) l = n - 1;
            memcpy(out, f, l);
            out[l] = 0;
        }
        char *slash = strrchr(out, '/');
        if (slash) memmove(out, slash + 1, strlen(slash + 1) + 1);
    }
    if (!out[0]) {
        const char *p = url;
        const char *sch = strstr(p, "://");
        if (sch) p = sch + 3;
        int l = (int)strcspn(p, "?#");
        const char *last = NULL;
        for (const char *t = p; t < p + l; t++)
            if (*t == '/') last = t;
        if (last && last + 1 < p + l)
            pct_decode(last + 1, (int)(p + l - last - 1), out, n);
    }
    fat_clean(out);
    if (!out[0]) snprintf(out, n, "download");
}
