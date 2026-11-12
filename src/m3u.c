/* XLT - .m3u / .m3u8 playlists. See m3u.h.
 *
 * Playlists come from many places, so a song line can be:
 *   - a full PSP path: "ms0:/MUSIC/a.mp3" (the device's case is ignored);
 *   - from the root of the same device: "/MUSIC/a.mp3";
 *   - relative to the playlist's folder: "a.mp3", "../Artist/a.mp3";
 *   - a PC path: "C:\Users\me\Music\Artist\a.mp3". The PC's folders don't
 *     exist here, so the part from the first folder called MUSIC (any case)
 *     is used on the playlist's device; failing that, the file name next to
 *     the playlist;
 *   - a file:// address (with %20 and such), read like the paths above.
 * Backslashes count as slashes. Web addresses are skipped.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>
#include "m3u.h"
#include "textenc.h"

/* The device of a path ("ms0:" -> 4 bytes), 0 if it has none. */
static int dev_len(const char *p)
{
    const char *c = strchr(p, ':'), *s = strchr(p, '/');
    if (!c || (s && s < c) || c == p)
        return 0;
    return (int)(c - p) + 1;
}

/* Folds "dir/./x", "dir/../x" and "//" in the part after the device.
 * -1 if it is too long to fold. */
static int normalize(char *p)
{
    int d = dev_len(p);
    char *base = p + d, out[M3U_PATH * 4];
    int n = 0, starts[64], depth = 0;
    const char *s = base;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        const char *e = s;
        while (*e && *e != '/') e++;
        int len = (int)(e - s);
        if (len == 1 && s[0] == '.') {
            /* nothing */
        } else if (len == 2 && s[0] == '.' && s[1] == '.') {
            if (depth > 0) n = starts[--depth]; /* above the root stays at the root */
        } else {
            if (n + len + 2 >= (int)sizeof(out) || depth >= 64)
                return -1;
            starts[depth++] = n;
            out[n++] = '/';
            memcpy(out + n, s, len);
            n += len;
        }
        s = e;
    }
    if (n == 0) out[n++] = '/';
    out[n] = 0;
    if (d + n >= M3U_PATH * 4)
        return -1;
    strcpy(base, out); /* p is M3U_PATH * 4 bytes */
    return 0;
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* The playlist's folder ("ms0:/MUSIC/PLAYLISTS") and device ("ms0:"). */
static void m3u_dir(const char *m3u_path, char *dir, int cap, char *dev, int dcap)
{
    const char *sl = strrchr(m3u_path, '/');
    int n = sl ? (int)(sl - m3u_path) : 0;
    snprintf(dir, cap, "%.*s", n, m3u_path);
    int d = dev_len(m3u_path);
    snprintf(dev, dcap, "%.*s", d ? d : 4, d ? m3u_path : "ms0:");
    for (char *c = dev; *c; c++)
        *c = (char)tolower((unsigned char)*c);
}

int m3u_resolve(const char *line, const char *m3u_path, char *out, int cap)
{
    char p[M3U_PATH * 2], dir[M3U_PATH], dev[16];
    while (*line == ' ' || *line == '\t') line++;
    int n = (int)strlen(line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' || line[n - 1] == '\r' || line[n - 1] == '\n')) n--;
    if (n == 0 || line[0] == '#' || n >= (int)sizeof(p))
        return -1;
    int file_url = n > 7 && !strncasecmp(line, "file://", 7);
    if (!file_url) {
        const char *c = memchr(line, ':', n);
        if (c && c - line > 1 && c + 2 < line + n && c[1] == '/' && c[2] == '/')
            return -1; /* http://, rtsp://, ... */
    }
    /* copy, with \ as / and a file:// address decoded */
    int k = 0;
    for (int i = file_url ? 7 : 0; i < n; i++) {
        char ch = line[i];
        if (file_url && ch == '%' && i + 2 < n && hexval(line[i + 1]) >= 0 && hexval(line[i + 2]) >= 0) {
            ch = (char)(hexval(line[i + 1]) * 16 + hexval(line[i + 2]));
            i += 2;
        }
        p[k++] = ch == '\\' ? '/' : ch;
    }
    p[k] = 0;
    if (k == 0 || p[k - 1] == '/')
        return -1; /* a folder */
    char *q = p;
    if (file_url && q[0] == '/' && dev_len(q + 1)) q++; /* file:///C:/... */
    if (file_url && !strncasecmp(q, "localhost/", 10)) q += 9;

    m3u_dir(m3u_path, dir, sizeof(dir), dev, sizeof(dev));
    char full[M3U_PATH * 4];
    int d = dev_len(q);
    if (d == 2 && isalpha((unsigned char)q[0])) { /* a PC drive letter */
        const char *m = NULL;
        for (const char *s = q + 2; *s; s++)
            if (s[-1] == '/' && !strncasecmp(s, "MUSIC/", 6)) { m = s; break; }
        if (m) {
            snprintf(full, sizeof(full), "%s/MUSIC/%s", dev, m + 6);
        } else {
            const char *b = strrchr(q, '/');
            snprintf(full, sizeof(full), "%s/%s", dir, b ? b + 1 : q + 2);
        }
    } else if (d) {
        for (int i = 0; i < d; i++)
            q[i] = (char)tolower((unsigned char)q[i]);
        snprintf(full, sizeof(full), "%s", q);
    } else if (q[0] == '/') {
        snprintf(full, sizeof(full), "%s%s", dev, q);
    } else {
        snprintf(full, sizeof(full), "%s/%s", dir, q);
    }
    if (normalize(full) != 0)
        return -1;
    int len = (int)strlen(full);
    if (len >= cap)
        return -1;
    memcpy(out, full, len + 1);
    return 0;
}

/* The line in UTF-8: kept if it is, else read as Windows-1252. */
static void to_utf8(const char *s, int n, char *out, int cap)
{
    if (utf8_valid((const uint8_t *)s, n)) {
        if (n >= cap) n = cap - 1;
        memcpy(out, s, n);
        out[n] = 0;
        return;
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        uint32_t cp = enc_cp(ENC_1252, (uint8_t)s[i]);
        char b[4];
        int m = utf8_put(cp ? cp : '?', b);
        if (k + m >= cap) break;
        memcpy(out + k, b, m);
        k += m;
    }
    out[k] = 0;
}

int m3u_parse(const char *text, int len, const char *m3u_path, M3uFn fn, void *ctx)
{
    char line[M3U_PATH * 3], path[M3U_PATH], title[128] = "";
    int count = 0, i = 0;
    if (len >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) i = 3; /* a byte order mark */
    while (i < len) {
        int s = i;
        while (i < len && text[i] != '\n' && text[i] != '\r') i++;
        int n = i - s;
        while (i < len && (text[i] == '\n' || text[i] == '\r')) i++;
        if (n <= 0 || n > M3U_PATH * 2)
            continue;
        to_utf8(text + s, n, line, sizeof(line));
        if (line[0] == '#') {
            if (!strncasecmp(line, "#EXTINF:", 8)) {
                const char *c = strchr(line + 8, ',');
                c = c ? c + 1 : "";
                while (*c == ' ') c++;
                snprintf(title, sizeof(title), "%s", c);
                int t = (int)strlen(title);
                while (t > 0 && (title[t - 1] == ' ' || title[t - 1] == '\t')) title[--t] = 0;
            }
            continue; /* #EXTM3U and other comments */
        }
        if (m3u_resolve(line, m3u_path, path, sizeof(path)) == 0) {
            if (fn) fn(path, title, ctx);
            count++;
        }
        title[0] = 0; /* a title is for the line right after it */
    }
    return count;
}

void m3u_relative(const char *dir, const char *target, char *out, int cap)
{
    int dd = dev_len(dir), td = dev_len(target);
    if (!dd || dd != td || strncasecmp(dir, target, dd)) {
        snprintf(out, cap, "%s", target);
        return;
    }
    const char *a = dir + dd, *b = target + td;
    /* skip the folders they share (FAT ignores case) */
    for (;;) {
        while (*a == '/') a++;
        while (*b == '/') b++;
        const char *ae = a, *be = b;
        while (*ae && *ae != '/') ae++;
        while (*be && *be != '/') be++;
        if (!*a || !*be || ae - a != be - b || strncasecmp(a, b, ae - a))
            break;
        a = ae;
        b = be;
    }
    int k = 0;
    out[0] = 0;
    while (*a) { /* one ../ for each folder of dir left */
        while (*a == '/') a++;
        if (!*a) break;
        while (*a && *a != '/') a++;
        k += snprintf(out + k, k < cap ? cap - k : 0, "../");
    }
    snprintf(out + (k < cap ? k : cap - 1), k < cap ? cap - k : 1, "%s", b);
}

int m3u_write(char *out, int cap, const char *m3u_path, const char *const *paths, const char *const *titles, int n)
{
    char dir[M3U_PATH], dev[16], rel[M3U_PATH * 2];
    m3u_dir(m3u_path, dir, sizeof(dir), dev, sizeof(dev));
    int k = snprintf(out, cap, "#EXTM3U\n");
    for (int i = 0; i < n && k < cap; i++) {
        const char *t = titles && titles[i] ? titles[i] : "";
        if (t[0]) {
            k += snprintf(out + k, cap - k, "#EXTINF:-1,");
            for (; *t && k < cap - 1; t++)
                out[k++] = *t == '\n' || *t == '\r' ? ' ' : *t;
            if (k < cap) out[k] = 0;
            k += snprintf(out + k, k < cap ? cap - k : 0, "\n");
        }
        m3u_relative(dir, paths[i], rel, sizeof(rel));
        if (k < cap)
            k += snprintf(out + k, cap - k, "%s\n", rel);
    }
    return k < cap ? k : -1;
}

int m3u_is_playlist(const char *name)
{
    int n = (int)strlen(name);
    return (n > 4 && !strcasecmp(name + n - 4, ".m3u")) || (n > 5 && !strcasecmp(name + n - 5, ".m3u8"));
}
