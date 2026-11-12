/* XLT - search matching. */
#include <string.h>
#include "match.h"

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int is_word_char(int c) { c = lower(c); return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80; }

/* Position of the first occurrence of w[0..n) in t (case-insensitive) at or
 * after `from`, preferring an occurrence at a word start. *at_word set to 1
 * if the returned one starts a word. Returns -1 if none. */
static int find(const char *t, const char *w, int n, int *at_word)
{
    int first = -1;
    for (int i = 0; t[i]; i++) {
        int k = 0;
        while (k < n && t[i + k] && lower((unsigned char)t[i + k]) == lower((unsigned char)w[k]))
            k++;
        if (k < n)
            continue;
        int ws = i == 0 || !is_word_char((unsigned char)t[i - 1]);
        if (ws) {
            *at_word = 1;
            return i;
        }
        if (first < 0)
            first = i;
    }
    *at_word = 0;
    return first;
}

int match_score(const char *title, const char *query)
{
    while (*query == ' ')
        query++;
    if (!*query)
        return 0;

    int words = 0, word_starts = 0, first_pos = 1 << 20;
    for (const char *q = query; *q;) {
        while (*q == ' ')
            q++;
        int n = 0;
        while (q[n] && q[n] != ' ')
            n++;
        if (!n)
            break;
        int at_word, pos = find(title, q, n, &at_word);
        if (pos < 0)
            return 0;
        words++;
        word_starts += at_word;
        if (pos < first_pos)
            first_pos = pos;
        q += n;
    }

    /* Whole query as a prefix of the whole title (spaces included). */
    size_t ql = strlen(query);
    while (ql && query[ql - 1] == ' ')
        ql--;
    int prefix = 1;
    for (size_t i = 0; i < ql && prefix; i++)
        prefix = title[i] && lower((unsigned char)title[i]) == lower((unsigned char)query[i]);

    int score = 1000;
    if (prefix)
        score += 4000;
    if (word_starts == words)
        score += 2000;
    score += word_starts * 100;
    score -= first_pos < 100 ? first_pos : 100; /* earlier is better */
    return score;
}
