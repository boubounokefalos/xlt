/* The recorder's file: JPEG frames of a made-up screen in an MJPEG AVI, checked
 * with ffprobe and decoded back with ffmpeg (both must accept it as it is). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "avi.h"
#include "recjpeg.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int fwrite_all(void *ctx, const void *p, uint32_t n) { return fwrite(p, 1, n, ctx) == n ? 0 : -1; }
static int fpatch(void *ctx, uint32_t off, const void *p, uint32_t n)
{
    FILE *f = ctx;
    long end = ftell(f);
    if (fseek(f, (long)off, SEEK_SET) || fwrite(p, 1, n, f) != n || fseek(f, end, SEEK_SET))
        return -1;
    return 0;
}

static int fail_after;
static int failing_write(void *ctx, const void *p, uint32_t n)
{
    (void)ctx; (void)p;
    if ((int)n > fail_after) return -1;
    fail_after -= (int)n;
    return 0;
}
static int no_patch(void *ctx, uint32_t off, const void *p, uint32_t n) { (void)ctx; (void)off; (void)p; (void)n; return 0; }

/* 565 with red in the low bits, as the PSP's display has it. */
static uint16_t rgb(int r, int g, int b) { return (uint16_t)((r >> 3) | (g >> 2) << 5 | (b >> 3) << 11); }

static void screen(uint16_t *fb, int stride, int frame)
{
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++)
            fb[y * stride + x] = rgb(13, 17, 23);
    for (int y = 100; y < 160; y++)
        for (int x = frame * 8; x < frame * 8 + 60 && x < 480; x++)
            fb[y * stride + x] = rgb(230, 40, 40); /* a red square moving right */
}

static char *run(const char *cmd)
{
    static char buf[4096];
    FILE *p = popen(cmd, "r");
    if (!p) return "";
    size_t n = fread(buf, 1, sizeof(buf) - 1, p);
    buf[n] = 0;
    pclose(p);
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    const char *dir = argv[1];
    static uint16_t fb[512 * 272];
    char path[512], cmd[1200];
    snprintf(path, sizeof(path), "%s/rec.avi", dir);

    /* one frame on its own */
    screen(fb, 512, 10);
    uint8_t *jpeg;
    unsigned long len;
    CHECK(rec_jpeg(fb, 512, 480, 272, 70, &jpeg, &len) == 0 && len > 1000 && len < 60000, "a frame: %lu bytes", len);
    CHECK(jpeg && jpeg[0] == 0xFF && jpeg[1] == 0xD8, "starts like a JPEG");
    free(jpeg);

    /* a recording: 20 pictures, each followed by two repeats, at 30 fps */
    FILE *f = fopen(path, "wb+");
    Avi a;
    CHECK(avi_begin(&a, 480, 272, 30, fwrite_all, fpatch, f) == AVI_OK, "begin");
    for (int i = 0; i < 20; i++) {
        screen(fb, 512, i);
        CHECK(rec_jpeg(fb, 512, 480, 272, 70, &jpeg, &len) == 0, "frame %d", i);
        CHECK(avi_frame(&a, jpeg, (uint32_t)len) == AVI_OK, "write %d", i);
        CHECK(avi_frame(&a, NULL, 0) == AVI_OK && avi_frame(&a, NULL, 0) == AVI_OK, "repeats %d", i);
        free(jpeg);
    }
    CHECK(a.frames == 60 && a.pictures == 20, "%u frames, %u pictures", a.frames, a.pictures);
    CHECK(avi_end(&a) == AVI_OK, "end");
    fclose(f);

    snprintf(cmd, sizeof(cmd), "ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height,r_frame_rate "
             "-show_entries format=duration -of default=nw=1 '%s' 2>&1", path);
    char *p = run(cmd);
    CHECK(strstr(p, "codec_name=mjpeg"), "MJPEG: %s", p);
    CHECK(strstr(p, "width=480") && strstr(p, "height=272"), "480x272: %s", p);
    CHECK(strstr(p, "r_frame_rate=30/1"), "30 fps: %s", p);
    const char *d = strstr(p, "duration=");
    double dur = d ? atof(d + 9) : 0;
    CHECK(dur > 1.9 && dur < 2.1, "2 seconds long: %.3f", dur);
    snprintf(cmd, sizeof(cmd), "ffmpeg -v error -i '%s' -f null - 2>&1", path);
    p = run(cmd);
    CHECK(p[0] == 0, "ffmpeg decodes it without complaint: %s", p);
    snprintf(cmd, sizeof(cmd), "ffmpeg -v error -ss 1.52 -i '%s' -frames:v 1 -f rawvideo -pix_fmt rgb24 - 2>/dev/null | "
             "od -An -tu1 -j %d -N 3", path, (130 * 480 + 15 * 8 + 30) * 3);
    p = run(cmd);
    int r = 0, g = 0, b = 0;
    sscanf(p, "%d %d %d", &r, &g, &b);
    CHECK(r > 180 && g < 90 && b < 90, "frame 45 (a repeat of picture 15) has the red square where it was: %d %d %d", r, g, b);

    /* the stick filling up, and the 1 GB limit */
    fail_after = 5000;
    CHECK(avi_begin(&a, 480, 272, 30, failing_write, no_patch, NULL) == AVI_OK, "begin on a nearly full stick");
    static uint8_t big[8000];
    CHECK(avi_frame(&a, big, sizeof(big)) == AVI_ERR_WRITE, "a full stick is reported");
    CHECK(avi_frame(&a, NULL, 0) == AVI_ERR_WRITE, "and stays reported");
    avi_abandon(&a);
    fail_after = 1 << 30;
    CHECK(avi_begin(&a, 480, 272, 30, failing_write, no_patch, NULL) == AVI_OK, "begin");
    a.pos = AVI_MAX_BYTES - 100;
    CHECK(avi_frame(&a, big, sizeof(big)) == AVI_ERR_FULL, "stops before 1 GB");
    avi_abandon(&a);

    /* odd sizes are padded, and the index grows */
    snprintf(path, sizeof(path), "%s/rec_long.avi", dir);
    f = fopen(path, "wb+");
    CHECK(avi_begin(&a, 480, 272, 30, fwrite_all, fpatch, f) == AVI_OK, "begin");
    screen(fb, 512, 3);
    rec_jpeg(fb, 512, 480, 272, 70, &jpeg, &len);
    for (int i = 0; i < 3000; i++)
        avi_frame(&a, i % 50 ? NULL : jpeg, i % 50 ? 0 : (uint32_t)(len | 1) - (len & 1 ? 0 : 1));
    free(jpeg);
    CHECK(a.frames == 3000 && a.idx_cap >= 3000, "3000 frames, index of %u", a.idx_cap);
    CHECK(avi_end(&a) == AVI_OK, "end");
    fclose(f);
    snprintf(cmd, sizeof(cmd), "ffprobe -v error -show_entries format=duration -of csv=p=0 '%s' 2>&1", path);
    dur = atof(run(cmd));
    CHECK(dur > 99.9 && dur < 100.1, "100 seconds: %.3f", dur);

    if (fails) {
        printf("test_rec: %d failed\n", fails);
        return 1;
    }
    puts("test_rec: ok");
    return 0;
}
