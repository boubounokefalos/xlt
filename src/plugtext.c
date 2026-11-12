/* XLT - PLUGINS.TXT per-game logic. See plugtext.h. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "plugtext.h"

#define LINE_MAX_LEN 256

typedef struct {
    char runlevel[64];
    char path[PLUG_PATH];
    int on;
} Line;

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* Parses "runlevel, path, on|off" like ARK: comments skipped, trailing
 * comments after the third field cut. Returns 1 for a plugin line. */
static int parse(const char *src, int n, Line *l)
{
    char buf[LINE_MAX_LEN];
    if (n >= LINE_MAX_LEN) return 0;
    memcpy(buf, src, n);
    buf[n] = 0;
    char *s = trim(buf);
    if (!*s || s[0] == '#' || s[0] == ';' || !strncmp(s, "//", 2)) return 0;
    char *c1 = strchr(s, ','), *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
    if (!c2) return 0;
    *c1 = *c2 = 0;
    char *en = c2 + 1;
    for (char *p = en; *p; p++)
        if (*p == '#' || *p == ';' || !strncmp(p, "//", 2) || *p == ',') { *p = 0; break; }
    snprintf(l->runlevel, sizeof(l->runlevel), "%s", trim(s));
    snprintf(l->path, sizeof(l->path), "%s", trim(c1 + 1));
    en = trim(en);
    l->on = !strcasecmp(en, "on") || !strcasecmp(en, "true") || !strcmp(en, "1") || !strcasecmp(en, "enabled");
    for (char *p = l->runlevel; *p; p++) *p = (char)tolower((unsigned char)*p);
    return l->path[0] != 0;
}

/* ARK's matchingRunlevel() for a PSP game (umd/iso/PSN) or a PS1 game. */
static int matches(const char *rl, const char *id, int pops)
{
    if (!strcmp(rl, "all") || !strcmp(rl, "always")) return 1;
    if (strchr(rl, '/')) return 0; /* a homebrew path */
    if (id[0]) {
        char lid[16];
        int i = 0;
        for (; id[i] && i < 15; i++) lid[i] = (char)tolower((unsigned char)id[i]);
        lid[i] = 0;
        if (strstr(rl, lid)) return 1;
    }
    if (pops)
        return strstr(rl, "pops") || strstr(rl, "ps1") || strstr(rl, "psx");
    return strstr(rl, "umd") || strstr(rl, "psp") || strstr(rl, "game");
}

static int vsh_only(const char *rl)
{
    return !strcmp(rl, "vsh") || !strcmp(rl, "xmb") || !strcmp(rl, "launcher") ||
           !strcmp(rl, "app") || !strcmp(rl, "homebrew");
}

static int is_own(const Line *l, const char *id)
{
    return !strcasecmp(l->runlevel, id);
}

int plugtext_list(const char *const *texts, int nfiles, const char *id, int pops, PlugState *out, int max)
{
    int count = 0;
    static int any_game[PLUG_MAX]; /* plugin has a line that is not XMB/homebrew-only */
    for (int f = 0; f < nfiles; f++) {
        const char *s = texts[f], *next;
        for (; s && *s; s = next) {
            const char *nl = strchr(s, '\n');
            int n = nl ? (int)(nl - s) : (int)strlen(s);
            next = nl ? nl + 1 : s + n;
            int k = n;
            while (k > 0 && s[k - 1] == '\r') k--;
            Line l;
            if (!parse(s, k, &l)) continue;
            int i = 0;
            while (i < count && strcasecmp(out[i].path, l.path)) i++;
            if (i == count) {
                if (count == max) continue;
                memset(&out[i], 0, sizeof(out[i]));
                snprintf(out[i].path, sizeof(out[i].path), "%s", l.path);
                out[i].override = PLUG_GLOBAL;
                any_game[i] = 0;
                count++;
            }
            out[i].file = f;
            if (!vsh_only(l.runlevel)) any_game[i] = 1;
            if (id[0] && is_own(&l, id))
                out[i].override = l.on ? PLUG_ON : PLUG_OFF; /* the last one wins */
            else if (matches(l.runlevel, id, pops))
                out[i].global_on = l.on;
        }
    }
    /* Drop plugins that only ever load in the XMB or for homebrew. */
    int w = 0;
    for (int i = 0; i < count; i++)
        if (any_game[i]) {
            if (w != i) out[w] = out[i];
            w++;
        }
    return w;
}

static int put(char *out, int *len, int cap, const char *s, int n)
{
    if (*len + n >= cap) return -1;
    memcpy(out + *len, s, n);
    *len += n;
    return 0;
}

int plugtext_set(const char *text, const char *id, const char *path, int override, char *out, int cap)
{
    int len = 0, err = 0;
    const char *s = text ? text : "", *next;
    for (; *s; s = next) {
        const char *nl = strchr(s, '\n');
        int n = nl ? (int)(nl - s) : (int)strlen(s);
        next = nl ? nl + 1 : s + n;
        int k = n;
        while (k > 0 && s[k - 1] == '\r') k--;
        Line l;
        if (parse(s, k, &l) && is_own(&l, id) && !strcasecmp(l.path, path))
            continue; /* ours: replaced below */
        int blank = 1;
        for (int i = 0; i < k && blank; i++) blank = isspace((unsigned char)s[i]);
        if (blank) continue;
        err |= put(out, &len, cap, s, k);
        err |= put(out, &len, cap, "\n", 1);
    }
    if (override != PLUG_GLOBAL) {
        char line[LINE_MAX_LEN];
        int m = snprintf(line, sizeof(line), "%s, %s, %s\n", id, path, override == PLUG_ON ? "on" : "off");
        if (m >= (int)sizeof(line)) return -1;
        err |= put(out, &len, cap, line, m);
    }
    return err ? -1 : len;
}

/* ------------------------------------------------- global plugin lines */

static const char *const KNOWN_RL[] = {"all", "always", "game", "umd", "psp", "homebrew", "pops", "psx", "ps1",
                                       "vsh", "xmb", "launcher", "app"};

static int known_runlevel(const char *rl)
{
    for (unsigned i = 0; i < sizeof(KNOWN_RL) / sizeof(KNOWN_RL[0]); i++)
        if (!strcmp(rl, KNOWN_RL[i]))
            return 1;
    return 0;
}

int plugtext_lines(const char *text, int file, PlugLine *out, int max)
{
    int n = 0, line = 0;
    const char *p = text;
    while (*p && n < max) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        Line l;
        if (parse(p, len, &l) && known_runlevel(l.runlevel)) {
            PlugLine *o = &out[n++];
            snprintf(o->path, sizeof(o->path), "%s", l.path);
            snprintf(o->runlevel, sizeof(o->runlevel), "%.15s", l.runlevel); /* known names are short */
            o->on = l.on;
            o->file = file;
            o->line = line;
        }
        line++;
        if (!nl)
            break;
        p = nl + 1;
    }
    return n;
}

int plugtext_set_line(const char *text, int line, const char *runlevel, int on, char *out, int cap)
{
    int len = 0, cur = 0, done = 0;
    const char *p = text;
    if (cap > 0) out[0] = 0;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        if (cur == line) {
            Line l;
            if (!parse(p, n, &l))
                return -1; /* not a plugin line (the file changed under us) */
            /* keep the runlevel and path text as written; only the state changes */
            const char *c1 = memchr(p, ',', n);
            const char *c2 = c1 ? memchr(c1 + 1, ',', n - (int)(c1 + 1 - p)) : NULL;
            if (!c2)
                return -1;
            if (runlevel) { /* new runlevel, then the path field exactly as written */
                if (put(out, &len, cap, runlevel, (int)strlen(runlevel)) < 0) return -1;
                if (put(out, &len, cap, c1, (int)(c2 - c1)) < 0) return -1;
            } else if (put(out, &len, cap, p, (int)(c2 - p)) < 0) {
                return -1;
            }
            const char *st = on ? ", on" : ", off";
            if (put(out, &len, cap, st, (int)strlen(st)) < 0) return -1;
            if (n > 0 && p[n - 1] == '\r' && put(out, &len, cap, "\r", 1) < 0) return -1;
            done = 1;
        } else if (put(out, &len, cap, p, n) < 0) {
            return -1;
        }
        if (nl && put(out, &len, cap, "\n", 1) < 0)
            return -1;
        cur++;
        if (!nl)
            break;
        p = nl + 1;
    }
    if (!done)
        return -1;
    out[len] = 0;
    return len;
}

/* ---------------------------------------------- the in-game screenshot plugin */

static void base_lc(const char *path, char *out, int n)
{
    const char *b = path;
    for (const char *s = path; *s; s++)
        if (*s == '/' || *s == ':' || *s == '\\') b = s + 1;
    int k = 0;
    for (; *b && k < n - 1; b++) out[k++] = (char)tolower((unsigned char)*b);
    out[k] = 0;
}

int plugtext_mentions(const char *text, const char *name)
{
    char want[PLUG_PATH], have[PLUG_PATH];
    base_lc(name, want, sizeof(want));
    const char *p = text;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        Line l;
        if (parse(p, len, &l)) {
            base_lc(l.path, have, sizeof(have));
            if (!strcmp(have, want)) return 1;
        }
        p = nl ? nl + 1 : NULL;
    }
    return 0;
}

int plugtext_append(const char *text, const char *line, char *out, int cap)
{
    int n = (int)strlen(text), crlf = strstr(text, "\r\n") != NULL;
    const char *eol = crlf ? "\r\n" : "\n";
    int need = n + (int)strlen(line) + 4 + 1;
    if (need > cap) return -1;
    memcpy(out, text, n);
    if (n && text[n - 1] != '\n') { memcpy(out + n, eol, strlen(eol)); n += (int)strlen(eol); }
    int k = (int)strlen(line);
    memcpy(out + n, line, k);
    n += k;
    memcpy(out + n, eol, strlen(eol));
    n += (int)strlen(eol);
    out[n] = 0;
    return n;
}

int plugtext_is_screenshot(const char *path)
{
    static const char *const words[] = {"screenshot", "screenshoot", "scrnshot", "scrshot", "sshot", "snapshot",
                                        "screencap", "scrcap", "capture", "pspshot", "prxshot", NULL};
    char b[PLUG_PATH];
    base_lc(path, b, sizeof(b));
    if (!strcmp(b, "xltshot.prx")) return 0;
    for (int i = 0; words[i]; i++)
        if (strstr(b, words[i])) return 1;
    return 0;
}

int plugtext_off_screenshots(const char *text, char *out, int cap, char *names, int ncap)
{
    int count = 0, len = (int)strlen(text);
    if (len + 1 > cap) return -1;
    memcpy(out, text, len + 1);
    if (ncap > 0) names[0] = 0;
    PlugLine lines[PLUG_MAX];
    int n = plugtext_lines(text, 0, lines, PLUG_MAX);
    for (int i = 0; i < n; i++) {
        const PlugLine *l = &lines[i];
        if (!l->on || !plugtext_is_screenshot(l->path) || !strcmp(l->runlevel, "vsh") || !strcmp(l->runlevel, "launcher"))
            continue;
        /* one line at a time, over what the last change wrote; only that line's state changes */
        char *tmp = malloc(cap);
        if (!tmp) return -1;
        int r = plugtext_set_line(out, l->line, NULL, 0, tmp, cap);
        if (r < 0) { free(tmp); return -1; }
        memcpy(out, tmp, r + 1);
        free(tmp);
        len = r;
        char b[PLUG_PATH];
        base_lc(l->path, b, sizeof(b));
        int used = (int)strlen(names);
        if (ncap > 0) snprintf(names + used, ncap - used, "%s%s", used ? ", " : "", b);
        count++;
    }
    (void)len;
    return count;
}

int plugtext_resolve(const char *path, const char *ark_dir, char *out, int n)
{
    while (*path == ' ') path++;
    if (strchr(path, ':'))
        snprintf(out, n, "%s", path);
    else if (path[0] == '/')
        snprintf(out, n, "ms0:%s", path);
    else if (ark_dir && ark_dir[0])
        snprintf(out, n, "%s%s", ark_dir, path);
    else
        return -1;
    return 0;
}
