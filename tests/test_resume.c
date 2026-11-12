/* resume.c: the music player's saved place and its play order. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "resume.h"

typedef struct { int n; char p[4][64], t[4][64], a[4][64]; } Got;
static void got(const char *p, const char *t, const char *a, void *ctx)
{
    Got *g = ctx;
    snprintf(g->p[g->n], 64, "%s", p);
    snprintf(g->t[g->n], 64, "%s", t);
    snprintf(g->a[g->n], 64, "%s", a);
    g->n++;
}

int main(void)
{
    /* state round trip, and defaults */
    ResumeState st = {7, 61234, 1, REPEAT_ONE, "Song\twith tab", "Band\nX"}, back;
    char buf[512];
    int n = resume_write_state(buf, sizeof(buf), &st);
    assert(n > 0 && n == (int)strlen(buf));
    resume_parse_state(buf, &back);
    assert(back.track == 7 && back.ms == 61234 && back.shuffle == 1 && back.repeat == REPEAT_ONE);
    assert(!strcmp(back.title, "Song with tab") && !strcmp(back.artist, "Band X"));
    resume_parse_state("", &back);
    assert(back.track == -1 && back.ms == 0 && !back.shuffle && back.repeat == REPEAT_OFF && !back.title[0]);
    resume_parse_state("repeat=9\r\ntrack=-3\nms=-5\n", &back);
    assert(back.repeat == REPEAT_OFF && back.track == -1 && back.ms == 0);
    resume_parse_state("track=2\r\n", &back);
    assert(back.track == 2);
    assert(resume_write_state(buf, 10, &st) == -1);

    /* list round trip */
    int k = 0;
    k += resume_write_song(buf + k, sizeof(buf) - k, "ms0:/MUSIC/a.mp3", "A", "Band");
    k += resume_write_song(buf + k, sizeof(buf) - k, "ms0:/MUSIC/b\t.mp3", "", "");
    k += resume_write_song(buf + k, sizeof(buf) - k, "ef0:/MUSIC/\xCE\x91.mp3", "\xCE\x91", "");
    Got g = {0};
    assert(resume_parse_list(buf, k, got, &g) == 3);
    assert(!strcmp(g.p[0], "ms0:/MUSIC/a.mp3") && !strcmp(g.t[0], "A") && !strcmp(g.a[0], "Band"));
    assert(!strcmp(g.p[1], "ms0:/MUSIC/b .mp3") && !g.t[1][0]);
    assert(!strcmp(g.p[2], "ef0:/MUSIC/\xCE\x91.mp3"));
    memset(&g, 0, sizeof(g));
    assert(resume_parse_list("x.mp3\r\n\n\tno path\n", 18, got, &g) == 1 && !strcmp(g.p[0], "x.mp3"));
    assert(resume_write_song(buf, 8, "ms0:/MUSIC/a.mp3", "", "") == -1);

    /* play order: every song once, the chosen one first */
    int order[50], seen[50];
    unsigned seed = 1234;
    for (int round = 0; round < 20; round++) {
        int first = round * 7 % 50;
        play_order(order, 50, 1, first, &seed);
        assert(order[0] == first);
        memset(seen, 0, sizeof(seen));
        for (int i = 0; i < 50; i++) { assert(order[i] >= 0 && order[i] < 50); seen[order[i]]++; }
        for (int i = 0; i < 50; i++) assert(seen[i] == 1);
    }
    play_order(order, 50, 1, 3, &seed);
    int moved = 0;
    for (int i = 1; i < 50; i++) moved += order[i] != i;
    assert(moved > 10); /* mixed, not just one swap */
    play_order(order, 5, 0, 3, &seed);
    for (int i = 0; i < 5; i++) assert(order[i] == i);
    play_order(order, 1, 1, 0, &seed);
    assert(order[0] == 0);

    /* next / previous */
    assert(order_next(0, 3, REPEAT_OFF, 1) == 1);
    assert(order_next(2, 3, REPEAT_OFF, 1) == -1);
    assert(order_next(2, 3, REPEAT_ALL, 1) == 0);
    assert(order_next(1, 3, REPEAT_ONE, 1) == 1);
    assert(order_next(1, 3, REPEAT_ONE, 0) == 2);
    assert(order_next(2, 3, REPEAT_ONE, 0) == 0);
    assert(order_next(0, 0, REPEAT_ALL, 1) == -1);
    assert(order_prev(2, 3, REPEAT_OFF) == 1);
    assert(order_prev(0, 3, REPEAT_OFF) == 0);
    assert(order_prev(0, 3, REPEAT_ALL) == 2);

    puts("test_resume: ok");
    return 0;
}
