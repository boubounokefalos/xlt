/* Host test for thumb_from_frame: odd sizes and strides stay in bounds (ASan),
 * and a uniform frame packs to that colour. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/thumbconv.h"

int main(void)
{
    static const int sizes[][3] = {{480, 272, 512}, {720, 480, 768}, {144, 80, 144}, {2, 2, 2}, {1, 500, 8}, {500, 1, 512}};
    uint8_t out[THUMB_BYTES];
    int fails = 0;
    for (int k = 0; k < 6; k++) {
        int w = sizes[k][0], h = sizes[k][1], stride = sizes[k][2];
        uint8_t *f = malloc((size_t)stride * h * 4);
        for (int i = 0; i < stride * h; i++) { f[i * 4] = 255; f[i * 4 + 1] = 0; f[i * 4 + 2] = 0; f[i * 4 + 3] = 0; }
        memset(out, 0, sizeof(out));
        if (thumb_from_frame(f, stride, w, h, out) != 0) { printf("FAIL %dx%d\n", w, h); fails++; }
        /* every texel: opaque pure red in ABGR1555 = 0x801F */
        for (int i = 0; i < THUMB_W * THUMB_H; i++) {
            unsigned v = out[i * 2] | out[i * 2 + 1] << 8;
            if (v != 0x801F) { printf("FAIL %dx%d texel %d = %04X\n", w, h, i, v); fails++; break; }
        }
        free(f);
    }
    if (thumb_from_frame(NULL, 1, 1, 1, out) == 0) { printf("FAIL null accepted\n"); fails++; }
    if (fails) return 1;
    printf("test_thumbframe: all passed\n");
    return 0;
}
