/* Host test helper: compress an ISO to CSO v1, CSO v2 or ZSO.
 * Usage: mkciso cso1|cso2|zso ALIGN in.iso out */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>
#include <lz4.h>

#define BS 2048

static int deflate_raw(const uint8_t *in, uint8_t *out, int cap)
{
    z_stream z = {0};
    deflateInit2(&z, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    z.next_in = (uint8_t *)in; z.avail_in = BS;
    z.next_out = out; z.avail_out = cap;
    deflate(&z, Z_FINISH);
    int n = cap - z.avail_out;
    deflateEnd(&z);
    return n;
}

int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    const char *fmt = argv[1];
    int align = atoi(argv[2]);
    FILE *f = fopen(argv[3], "rb");
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    rewind(f);
    uint8_t *iso = malloc(total);
    fread(iso, 1, total, f);
    fclose(f);

    uint32_t blocks = (total + BS - 1) / BS;
    uint32_t *idx = calloc(blocks + 1, 4);
    FILE *o = fopen(argv[4], "wb");
    uint8_t hdr[24] = {0};
    memcpy(hdr, strcmp(fmt, "zso") ? "CISO" : "ZISO", 4);
    hdr[4] = 24;
    uint64_t t = total;
    memcpy(hdr + 8, &t, 8);
    uint32_t bs = BS;
    memcpy(hdr + 16, &bs, 4);
    hdr[20] = strcmp(fmt, "cso2") ? 1 : 2;
    hdr[21] = align;
    fwrite(hdr, 1, 24, o);
    fwrite(idx, 4, blocks + 1, o);
    long pos = ftell(o);
    uint8_t cbuf[BS * 2];
    for (uint32_t b = 0; b < blocks; b++) {
        while (pos & ((1 << align) - 1)) { fputc(0, o); pos++; }
        int lz = !strcmp(fmt, "zso") || (!strcmp(fmt, "cso2") && (b % 3 == 0));
        int n = lz ? LZ4_compress_default((char *)iso + b * BS, (char *)cbuf, BS, sizeof(cbuf))
                   : deflate_raw(iso + b * BS, cbuf, sizeof(cbuf));
        uint32_t flag = 0;
        if (n >= BS || n <= 0) { /* store */
            memcpy(cbuf, iso + b * BS, BS);
            n = BS;
            flag = strcmp(fmt, "cso2") ? 0x80000000u : 0;
        } else if (!strcmp(fmt, "cso2") && lz) {
            flag = 0x80000000u;
        }
        idx[b] = (uint32_t)(pos >> align) | flag;
        fwrite(cbuf, 1, n, o);
        pos += n;
    }
    while (pos & ((1 << align) - 1)) { fputc(0, o); pos++; }
    idx[blocks] = (uint32_t)(pos >> align);
    fseek(o, 24, SEEK_SET);
    fwrite(idx, 4, blocks + 1, o);
    fclose(o);
    return 0;
}
