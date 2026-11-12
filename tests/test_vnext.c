/* Up next: which video follows which in a folder. */
#include <stdio.h>
#include <string.h>
#include "vnext.h"

static int fails;
static void check(const char *what, const char *me, const char *const *names, const char *want)
{
    char best[200] = "";
    for (int i = 0; names[i]; i++)
        vnext_consider(me, names[i], best, sizeof(best));
    int ok = !strcmp(best, want);
    printf("%s  %s (got \"%s\")\n", ok ? "OK  " : "FAIL", what, best);
    fails += !ok;
}

int main(void)
{
    const char *show[] = {"Episode 10.mp4", "Episode 2.mp4", "Episode 1.mp4", "Episode 1.srt", "cover.jpg",
                          "Episode 3.MP4", ".Episode 4.mp4", "Episode 11.pmf", NULL};
    check("2 follows 1, not 10", "Episode 1.mp4", show, "Episode 2.mp4");
    check("3 (upper-case .MP4) follows 2", "Episode 2.mp4", show, "Episode 3.MP4");
    check("10 follows 3, hidden .file skipped", "Episode 3.MP4", show, "Episode 10.mp4");
    check("a .pmf counts", "Episode 10.mp4", show, "Episode 11.pmf");
    check("nothing after the last", "Episode 11.pmf", show, "");
    const char *one[] = {"Film.mp4", "Film.srt", NULL};
    check("a lone film has no next", "Film.mp4", one, "");
    const char *ext[] = {"a.m4v", "b.avi", "c.mkv", "d.m4v", NULL};
    check("other formats are skipped", "a.m4v", ext, "d.m4v");
    printf(fails ? "test_vnext: %d failed\n" : "test_vnext: ok\n", fails);
    return fails != 0;
}
