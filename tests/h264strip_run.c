/* Weighted prediction stripper: rewrites an MP4's H.264 track into an Annex B
 * stream (SPS, the new PPS, every sample) for ffmpeg to decode and compare.
 *   h264strip_run IN.mp4 OUT.h264  ->  prints "needs N, stripped N, failed N" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "mp4.h"
#include "h264strip.h"

static const uint8_t SC[4] = {0, 0, 0, 1};

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    Mp4 m;
    const char *why = NULL;
    if (mp4_open(&m, argv[1], &why) != 0) { printf("open: %s\n", why); return 1; }
    H264Strip s;
    int needs = h264_strip_init(&s, m.sps, m.sps_size, m.pps, m.pps_size);
    FILE *o = fopen(argv[2], "wb");
    int fd = open(argv[1], O_RDONLY);
    if (!o || fd < 0) return 1;
    uint8_t pps[512];
    int pn = needs == 1 ? h264_strip_pps(m.pps, m.pps_size, pps, sizeof(pps)) : -1;
    fwrite(SC, 1, 4, o); fwrite(m.sps, 1, m.sps_size, o);
    fwrite(SC, 1, 4, o);
    if (pn > 0) fwrite(pps, 1, pn, o); else fwrite(m.pps, 1, m.pps_size, o);
    int ok = 0, failed = 0, smaller = 0;
    for (uint32_t i = 0; i < m.video.count; i++) {
        uint32_t n = mp4_size(&m.video, i);
        uint8_t *in = malloc(n), *out = malloc(n + n / 4 + 64);
        if (pread(fd, in, n, (off_t)mp4_offset(&m.video, i)) != (ssize_t)n) return 1;
        int k = needs == 1 ? h264_strip_sample(&s, in, n, m.nal_length, out, n + n / 4 + 64) : -1;
        const uint8_t *p = k >= 0 ? out : in;
        uint32_t len = k >= 0 ? (uint32_t)k : n;
        if (needs == 1) { if (k >= 0) ok++; else failed++; }
        if (k >= 0 && (uint32_t)k < n) smaller++;
        for (uint32_t pos = 0; pos + m.nal_length <= len;) { /* sizes -> start codes */
            uint32_t l = 0;
            for (int b = 0; b < m.nal_length; b++) l = l << 8 | p[pos + b];
            pos += m.nal_length;
            fwrite(SC, 1, 4, o); fwrite(p + pos, 1, l, o);
            pos += l;
        }
        free(in); free(out);
    }
    fclose(o); close(fd);
    printf("needs %d, stripped %d, failed %d, smaller %d, of %u samples\n", needs, ok, failed, smaller, m.video.count);
    mp4_close(&m);
    return failed != 0;
}
