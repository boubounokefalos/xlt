/* vplace.c: where each video was stopped. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vplace.h"

static VPlaces v, w;

int main(void)
{
    int dur;
    vp_set(&v, "ms0:/VIDEO/a.mp4", 600000, 1260000);   /* 10 of 21 min */
    vp_set(&v, "ms0:/VIDEO/b.mp4", 5000, 1260000);     /* barely started: not kept */
    vp_set(&v, "ms0:/VIDEO/c.mp4", 1220000, 1260000);  /* the last minute: watched */
    vp_set(&v, "ms0:/VIDEO/d.mp4", 95000, 100000);     /* the last 10% */
    vp_set(&v, "ms0:/VIDEO/e.mp4", 30000, 0);          /* length unknown: kept */
    assert(v.n == 2 && !strcmp(v.p[0].path, "ms0:/VIDEO/e.mp4"));
    assert(vp_get(&v, "ms0:/VIDEO/a.mp4", &dur) == 600000 && dur == 1260000);
    assert(vp_get(&v, "ms0:/VIDEO/b.mp4", &dur) == 0 && dur == 0);
    vp_set(&v, "ms0:/VIDEO/a.mp4", 700000, 1260000);   /* again: moves first, no duplicate */
    assert(v.n == 2 && !strcmp(v.p[0].path, "ms0:/VIDEO/a.mp4") && v.p[0].ms == 700000);
    vp_set(&v, "ms0:/VIDEO/a.mp4", 1250000, 1260000);  /* watched to the end: forgotten */
    assert(v.n == 1 && vp_get(&v, "ms0:/VIDEO/a.mp4", NULL) == 0);

    /* round trip, including a name with spaces and Greek */
    vp_set(&v, "ms0:/VIDEO/My Show \xCE\x91 s01e02.mp4", 123456, 1300000);
    char buf[4096];
    int n = vp_write(&v, buf, sizeof(buf));
    assert(n > 0 && n == (int)strlen(buf));
    vp_parse(&w, buf, n);
    assert(w.n == 2 && vp_get(&w, "ms0:/VIDEO/My Show \xCE\x91 s01e02.mp4", &dur) == 123456 && dur == 1300000);
    assert(vp_write(&v, buf, 10) == -1);

    /* the oldest goes when it's full; junk lines and duplicates are skipped */
    VPlaces f = {0};
    char p[64];
    for (int i = 0; i < VP_MAX + 5; i++) {
        snprintf(p, sizeof(p), "ms0:/VIDEO/%d.mp4", i);
        vp_set(&f, p, 60000, 0);
    }
    assert(f.n == VP_MAX && vp_get(&f, "ms0:/VIDEO/0.mp4", NULL) == 0 && vp_get(&f, p, NULL) == 60000);
    const char *junk = "garbage\n60000\t0\tms0:/x.mp4\r\n70000\t0\tms0:/x.mp4\n0\t5\tms0:/zero.mp4\n1\t2\t\n";
    vp_parse(&w, junk, (int)strlen(junk));
    assert(w.n == 1 && vp_get(&w, "ms0:/x.mp4", NULL) == 60000);
    vp_parse(&w, "", 0);
    assert(w.n == 0);
    puts("test_vplace: ok");
    return 0;
}
