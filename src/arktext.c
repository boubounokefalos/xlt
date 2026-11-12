/* XLT - SETTINGS.TXT clock lines as pure text functions (host-testable).
 * Lines are "runlevel, key, on|off"; a game ID as runlevel scopes the line
 * to that game. Only clock lines for the requested runlevel are replaced;
 * every other line is kept byte for byte (blank lines are dropped). */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "ark.h"
#include "arktext.h"

/* --------------------------------------------------------------- parsing */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

/* Splits "a, b, c" into 3 trimmed parts in `tmp`. Returns 1 if it has exactly 3. */
static int split3(const char *line, char *tmp, int n, char **p)
{
    snprintf(tmp, n, "%s", line);
    char *c1 = strchr(tmp, ','), *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
    if (!c1 || !c2 || strchr(c2 + 1, ','))
        return 0;
    *c1 = *c2 = 0;
    p[0] = trim(tmp);
    p[1] = trim(c1 + 1);
    p[2] = trim(c2 + 1);
    return 1;
}

/* CLOCK_* for a clock key, or -1 if the key is not about the clock. */
static int clock_of_key(const char *k)
{
    if (!strcasecmp(k, "cpuclock:133") || !strcasecmp(k, "powersave")) return CLOCK_133;
    if (!strcasecmp(k, "cpuclock:222") || !strcasecmp(k, "defaultclock")) return CLOCK_222;
    if (!strcasecmp(k, "cpuclock:333") || !strcasecmp(k, "overclock")) return CLOCK_333;
    if (!strncasecmp(k, "cpuclock", 8)) return CLOCK_AUTO; /* other speeds: treated as ours to replace */
    return -1;
}

static int is_on(const char *v)
{
    return !strcasecmp(v, "on") || !strcmp(v, "1") || !strcasecmp(v, "enabled") || !strcasecmp(v, "true");
}

/* Does this runlevel cover `id` (or "all games" when id is NULL)? */
static int level_matches(const char *rl, const char *id, int strict)
{
    if (id)
        return !strcasecmp(rl, id);
    if (!strcasecmp(rl, "game"))
        return 1;
    return !strict && (!strcasecmp(rl, "always") || !strcasecmp(rl, "all"));
}

int arktext_get_clock(const char *text, const char *id)
{
    int clock = CLOCK_AUTO;
    const char *line = text;
    while (*line) {
        const char *nl = strchr(line, '\n');
        int n = nl ? (int)(nl - line) : (int)strlen(line);
        char raw[160], tmp[160], *p[3];
        snprintf(raw, sizeof(raw), "%.*s", n < 159 ? n : 159, line);
        char *cr = strchr(raw, '\r');
        if (cr) *cr = 0;
        if (split3(raw, tmp, sizeof(tmp), p)) {
            int c = clock_of_key(p[1]);
            if (c >= 0 && level_matches(p[0], id, 0) && is_on(p[2]))
                clock = c; /* the last matching line wins */
        }
        line += n + (nl ? 1 : 0);
    }
    return clock;
}

static int put(char *out, int *len, int cap, const char *s, int n)
{
    if (*len + n >= cap)
        return -1;
    memcpy(out + *len, s, n);
    *len += n;
    return 0;
}

int arktext_set_clock(const char *text, const char *id, int clock, int ark4, char *out, int cap)
{
    int len = 0, err = 0;
    char buf[200];
    const char *line = text;
    while (*line) {
        const char *nl = strchr(line, '\n');
        int n = nl ? (int)(nl - line) : (int)strlen(line);
        int keep = n;
        while (keep > 0 && (line[keep - 1] == '\r'))
            keep--;
        char raw[160], tmp[160], *p[3];
        int parsed = 0;
        if (keep < (int)sizeof(raw)) {
            snprintf(raw, sizeof(raw), "%.*s", keep, line);
            parsed = split3(raw, tmp, sizeof(tmp), p) && clock_of_key(p[1]) >= 0;
        }
        if (parsed && level_matches(p[0], id, 1)) {
            /* replaced below */
        } else if (parsed && !id && (!strcasecmp(p[0], "always") || !strcasecmp(p[0], "all"))) {
            /* "always" also covers games: keep it for the XMB only */
            int m = snprintf(buf, sizeof(buf), "vsh, %s, %s\n", p[1], p[2]);
            err |= put(out, &len, cap, buf, m);
        } else if (keep >= (int)sizeof(raw) || trim(raw)[0]) {
            err |= put(out, &len, cap, line, keep);
            err |= put(out, &len, cap, "\n", 1);
        }
        line += n + (nl ? 1 : 0);
    }
    if (clock > CLOCK_AUTO && clock < CLOCK_COUNT) {
        static const char *k5[] = {"", "cpuclock:133", "cpuclock:222", "cpuclock:333"};
        static const char *k4[] = {"", "powersave", "defaultclock", "overclock"};
        int m = snprintf(buf, sizeof(buf), "%s, %s, on\n", id ? id : "game", ark4 ? k4[clock] : k5[clock]);
        err |= put(out, &len, cap, buf, m);
    }
    return err ? -1 : len;
}
