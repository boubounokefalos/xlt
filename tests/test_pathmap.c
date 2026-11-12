/* pathmap.c: a remembered number per file (picture rotations). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pathmap.h"

static PathMap m, b;

int main(void)
{
    pm_set(&m, "ms0:/PICTURE/a.jpg", 1);
    pm_set(&m, "ms0:/PICTURE/b.jpg", 3);
    assert(m.n == 2 && pm_get(&m, "ms0:/PICTURE/a.jpg") == 1 && pm_get(&m, "ms0:/PICTURE/b.jpg") == 3);
    assert(pm_get(&m, "ms0:/PICTURE/c.jpg") == 0);
    pm_set(&m, "ms0:/PICTURE/a.jpg", 2);            /* changed: first again, once */
    assert(m.n == 2 && !strcmp(m.e[0].path, "ms0:/PICTURE/a.jpg") && m.e[0].value == 2);
    pm_set(&m, "ms0:/PICTURE/b.jpg", 0);            /* back upright: forgotten */
    assert(m.n == 1 && pm_get(&m, "ms0:/PICTURE/b.jpg") == 0);
    pm_set(&m, "ms0:/PICTURE/\xCE\xB4 photo.jpg", 3);
    char buf[2048];
    int n = pm_write(&m, buf, sizeof(buf));
    assert(n > 0 && n == (int)strlen(buf));
    pm_parse(&b, buf, n);
    assert(b.n == 2 && pm_get(&b, "ms0:/PICTURE/\xCE\xB4 photo.jpg") == 3 && pm_get(&b, "ms0:/PICTURE/a.jpg") == 2);
    assert(pm_write(&m, buf, 8) == -1);
    const char *junk = "x\n2\tms0:/a.jpg\r\n3\tms0:/a.jpg\n0\tms0:/zero.jpg\n1\t\n";
    pm_parse(&b, junk, (int)strlen(junk));
    assert(b.n == 1 && pm_get(&b, "ms0:/a.jpg") == 2);
    PathMap f = {0};
    char p[32];
    for (int i = 0; i < PM_MAX + 3; i++) {
        snprintf(p, sizeof(p), "ms0:/%d.jpg", i);
        pm_set(&f, p, 1);
    }
    assert(f.n == PM_MAX && pm_get(&f, "ms0:/0.jpg") == 0 && pm_get(&f, p) == 1);
    puts("test_pathmap: ok");
    return 0;
}
