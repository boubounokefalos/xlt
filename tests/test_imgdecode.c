/* Host test for the streaming picture decoder: sizes, crop/fit maths and pixels
 * against Pillow references (exact for PNG/BMP, within JPEG tolerance). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>
#include "../src/imgdecode.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("OK   %s\n", msg); else { printf("FAIL %s\n", msg); fails++; } } while (0)

static uint8_t *load_ref(const char *path, int *w, int *h)
{
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, path)) return NULL;
    img.format = PNG_FORMAT_RGBA;
    uint8_t *b = malloc(PNG_IMAGE_SIZE(img));
    png_image_finish_read(&img, NULL, b, 0, NULL);
    *w = img.width; *h = img.height;
    return b;
}

/* max per-channel difference between decoded (stride ow) and the reference */
static int compare(const char *dir, const char *file, const char *ref, int mode, int ow, int oh, int tol)
{
    char p[256], r[256];
    snprintf(p, sizeof(p), "%s/%s", dir, file);
    snprintf(r, sizeof(r), "%s/%s", dir, ref);
    uint8_t *out = calloc((size_t)ow * oh, 4);
    int rw, rh, rfw, rfh;
    int res = img_decode(p, mode, ow, oh, out, &rw, &rh);
    uint8_t *want = load_ref(r, &rfw, &rfh);
    int worst = 999;
    if (res == 0 && want && rw == rfw && rh == rfh) {
        worst = 0;
        for (int y = 0; y < rh; y++)
            for (int x = 0; x < rw * 4; x++) {
                int d = abs(out[(size_t)y * ow * 4 + x] - want[(size_t)y * rw * 4 + x]);
                if (d > worst) worst = d;
            }
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "%s -> %dx%d (want %dx%d), max diff %d <= %d", file, rw, rh, rfw, rfh, worst, tol);
    CHECK(res == 0 && worst <= tol, msg);
    free(out);
    free(want);
    return worst;
}

int main(int argc, char **argv)
{
    const char *d = argc > 1 ? argv[1] : "build/pictures";
    compare(d, "photo.jpg", "photo_cover.png", IMG_COVER, 144, 80, 40);  /* JPEG + 1/8 DCT scaling */
    compare(d, "photo.jpg", "photo_fit.png", IMG_FIT, 480, 272, 40);
    compare(d, "pic.png", "pic_fit.png", IMG_FIT, 480, 272, 0);          /* never enlarged: 300x200 */
    compare(d, "pic_interlaced.png", "pic_fit.png", IMG_FIT, 480, 272, 0);
    compare(d, "shot.bmp", "shot_cover.png", IMG_COVER, 144, 80, 0);      /* bottom-up 24-bit BMP */
    compare(d, "tall.png", "tall_fit.png", IMG_FIT, 480, 272, 0);         /* portrait: height-limited */

    uint8_t buf[144 * 80 * 4];
    int rw, rh;
    char p[256];
    snprintf(p, sizeof(p), "%s/broken.jpg", d);
    CHECK(img_decode(p, IMG_COVER, 144, 80, buf, &rw, &rh) != 0, "truncated JPEG reports an error");
    snprintf(p, sizeof(p), "%s/notapicture.png", d);
    CHECK(img_decode(p, IMG_COVER, 144, 80, buf, &rw, &rh) != 0, "non-picture rejected");
    CHECK(img_decode("/nonexistent.png", IMG_COVER, 144, 80, buf, &rw, &rh) != 0, "missing file rejected");
    CHECK(img_is_picture("a.JPG") && img_is_picture("b.jpeg") && img_is_picture("c.Png") && img_is_picture("d.bmp")
          && !img_is_picture("e.iso") && !img_is_picture("noext"), "extensions recognised");
    CHECK(!strcmp(img_format_name("x.jpeg"), "JPEG") && !strcmp(img_format_name("x.BMP"), "BMP"), "format names");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
