/* XLT - H.264 weighted prediction stripper. See h264strip.h.
 * Syntax per ITU-T H.264 7.3.2.1 (SPS), 7.3.2.2 (PPS), 7.3.3 (slice header),
 * 7.3.3.1-7.3.3.3 (ref_pic_list_modification, pred_weight_table, dec_ref_pic_marking). */
#include <stdlib.h>
#include <string.h>
#include "h264strip.h"

/* ------------------------------------------------------------- bit I/O */

typedef struct {
    const uint8_t *p;
    int n;     /* bytes */
    long pos;  /* bits */
    int err;
} Rd;

static int rd_bit(Rd *r)
{
    if (r->pos >= (long)r->n * 8) {
        r->err = 1;
        return 0;
    }
    int b = r->p[r->pos >> 3] >> (7 - (r->pos & 7)) & 1;
    r->pos++;
    return b;
}

static uint32_t rd_bits(Rd *r, int k)
{
    uint32_t v = 0;
    while (k-- > 0)
        v = v << 1 | rd_bit(r);
    return v;
}

static uint32_t rd_ue(Rd *r)
{
    int zeros = 0;
    while (!rd_bit(r)) {
        if (r->err || ++zeros > 31) {
            r->err = 1;
            return 0;
        }
    }
    return zeros ? ((1u << zeros) - 1) + rd_bits(r, zeros) : 0;
}

static int32_t rd_se(Rd *r)
{
    uint32_t k = rd_ue(r);
    return k & 1 ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

typedef struct {
    uint8_t *p;
    int cap;  /* bytes */
    long pos; /* bits */
    int err;
} Wr;

static void wr_bit(Wr *w, int b)
{
    if (w->pos >= (long)w->cap * 8) {
        w->err = 1;
        return;
    }
    uint8_t m = (uint8_t)(0x80 >> (w->pos & 7));
    if (b) w->p[w->pos >> 3] |= m;
    else w->p[w->pos >> 3] &= (uint8_t)~m;
    w->pos++;
}

/* Copies bits [from, to) of r's buffer to w. */
static void copy_bits(Wr *w, const Rd *r, long from, long to)
{
    for (long i = from; i < to && !w->err; i++)
        wr_bit(w, r->p[i >> 3] >> (7 - (i & 7)) & 1);
}

/* ------------------------------------------------------- NAL <-> RBSP */

/* NAL payload (after the header byte) to RBSP: emulation prevention bytes out. */
static int to_rbsp(const uint8_t *in, int n, uint8_t *out, int cap)
{
    int k = 0, zeros = 0;
    for (int i = 0; i < n; i++) {
        if (zeros >= 2 && in[i] == 3) {
            zeros = 0;
            continue;
        }
        if (k >= cap)
            return -1;
        out[k++] = in[i];
        zeros = in[i] == 0 ? zeros + 1 : 0;
    }
    return k;
}

/* RBSP to NAL payload: 00 00 0x (x <= 3) gets an 03 in between. */
static int to_nal(const uint8_t *in, int n, uint8_t *out, int cap)
{
    int k = 0, zeros = 0;
    for (int i = 0; i < n; i++) {
        if (zeros >= 2 && in[i] <= 3) {
            if (k >= cap)
                return -1;
            out[k++] = 3;
            zeros = 0;
        }
        if (k >= cap)
            return -1;
        out[k++] = in[i];
        zeros = in[i] == 0 ? zeros + 1 : 0;
    }
    if (k > 0 && out[k - 1] == 0) { /* an RBSP ending in 0x00 (cabac_zero_word) gets a final 03 */
        if (k >= cap)
            return -1;
        out[k++] = 3;
    }
    return k;
}

/* ---------------------------------------------------------- SPS / PPS */

static void skip_scaling_list(Rd *r, int size)
{
    int last = 8, next = 8;
    for (int j = 0; j < size && !r->err; j++) {
        if (next)
            next = (last + rd_se(r) + 256) % 256;
        last = next ? next : last;
    }
}

static int parse_sps(H264Strip *s, const uint8_t *sps, int n)
{
    if (n < 4 || (sps[0] & 0x1F) != 7)
        return -1;
    uint8_t *b = malloc(n);
    if (!b)
        return -1;
    int len = to_rbsp(sps + 1, n - 1, b, n);
    Rd r = {b, len < 0 ? 0 : len, 0, 0};
    int profile = (int)rd_bits(&r, 8);
    rd_bits(&r, 16); /* constraint flags, level */
    rd_ue(&r);       /* sps id */
    s->chroma_array_type = 1;
    s->separate_colour_plane = 0;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        int cf = (int)rd_ue(&r);
        if (cf == 3)
            s->separate_colour_plane = rd_bit(&r);
        s->chroma_array_type = s->separate_colour_plane ? 0 : cf;
        rd_ue(&r);  /* bit depth luma */
        rd_ue(&r);  /* bit depth chroma */
        rd_bit(&r); /* qpprime_y_zero_transform_bypass */
        if (rd_bit(&r)) /* seq_scaling_matrix_present */
            for (int i = 0; i < (cf != 3 ? 8 : 12); i++)
                if (rd_bit(&r))
                    skip_scaling_list(&r, i < 6 ? 16 : 64);
    }
    s->log2_max_frame_num = (int)rd_ue(&r) + 4;
    s->poc_type = (int)rd_ue(&r);
    if (s->poc_type == 0) {
        s->log2_max_poc_lsb = (int)rd_ue(&r) + 4;
    } else if (s->poc_type == 1) {
        s->delta_poc_always_zero = rd_bit(&r);
        rd_se(&r); /* offset_for_non_ref_pic */
        rd_se(&r); /* offset_for_top_to_bottom_field */
        uint32_t cycle = rd_ue(&r);
        for (uint32_t i = 0; i < cycle && i < 256 && !r.err; i++)
            rd_se(&r);
    }
    rd_ue(&r);  /* max_num_ref_frames */
    rd_bit(&r); /* gaps_in_frame_num_allowed */
    rd_ue(&r);  /* width in mbs */
    rd_ue(&r);  /* height in map units */
    s->frame_mbs_only = rd_bit(&r);
    int err = r.err || s->log2_max_frame_num > 16 || s->poc_type > 2 || s->log2_max_poc_lsb > 16;
    free(b);
    return err ? -1 : 0;
}

/* Reads the PPS up to the weighting fields; *wpos gets the bit position of
 * weighted_pred_flag in the RBSP. */
static int parse_pps(H264Strip *s, const uint8_t *rbsp, int len, long *wpos)
{
    Rd r = {rbsp, len, 0, 0};
    rd_ue(&r); /* pps id */
    rd_ue(&r); /* sps id */
    s->cabac = rd_bit(&r);
    s->bottom_field_poc = rd_bit(&r);
    if (rd_ue(&r) != 0) /* slice groups (FMO): Baseline only, never weighted */
        return -1;
    s->num_ref_l0 = (int)rd_ue(&r) + 1;
    s->num_ref_l1 = (int)rd_ue(&r) + 1;
    *wpos = r.pos;
    s->weighted_pred = rd_bit(&r);
    s->weighted_bipred = (int)rd_bits(&r, 2);
    rd_se(&r); /* pic_init_qp */
    rd_se(&r); /* pic_init_qs */
    rd_se(&r); /* chroma_qp_index_offset */
    s->deblocking_control = rd_bit(&r);
    rd_bit(&r); /* constrained_intra_pred */
    s->redundant_pic_cnt = rd_bit(&r);
    return r.err || s->num_ref_l0 > 32 || s->num_ref_l1 > 32 ? -1 : 0;
}

int h264_strip_init(H264Strip *s, const uint8_t *sps, int sps_n, const uint8_t *pps, int pps_n)
{
    memset(s, 0, sizeof(*s));
    if (parse_sps(s, sps, sps_n) != 0 || pps_n < 2 || (pps[0] & 0x1F) != 8)
        return -1;
    uint8_t *b = malloc(pps_n);
    if (!b)
        return -1;
    int len = to_rbsp(pps + 1, pps_n - 1, b, pps_n);
    long wpos;
    int r = len > 0 ? parse_pps(s, b, len, &wpos) : -1;
    free(b);
    if (r != 0)
        return -1;
    s->ok = 1;
    return s->weighted_pred == 1 || s->weighted_bipred == 1;
}

int h264_strip_pps(const uint8_t *pps, int n, uint8_t *out, int cap)
{
    if (n < 2 || cap < 2)
        return -1;
    uint8_t *b = malloc(n);
    if (!b)
        return -1;
    int len = to_rbsp(pps + 1, n - 1, b, n);
    H264Strip t;
    long wpos;
    if (len <= 0 || parse_pps(&t, b, len, &wpos) != 0) {
        free(b);
        return -1;
    }
    /* weighted_pred_flag (1 bit) and weighted_bipred_idc (2 bits): same length, so in place.
     * Implicit bipred (2) has no syntax in the slices and is kept. */
    Wr w = {b, len, wpos, 0};
    wr_bit(&w, 0);
    if (t.weighted_bipred == 1) {
        wr_bit(&w, 0);
        wr_bit(&w, 0);
    }
    out[0] = pps[0];
    int k = to_nal(b, len, out + 1, cap - 1);
    free(b);
    return k < 0 ? -1 : k + 1;
}

/* ------------------------------------------------------------- slices */

enum { SL_P = 0, SL_B = 1, SL_I = 2, SL_SP = 3, SL_SI = 4 };

static void skip_ref_list_mod(Rd *r)
{
    if (!rd_bit(r)) /* ref_pic_list_modification_flag */
        return;
    for (int k = 0; k < 100 && !r->err; k++) {
        uint32_t idc = rd_ue(r);
        if (idc == 3)
            return;
        if (idc > 5) { /* 4/5 are MVC only */
            r->err = 1;
            return;
        }
        rd_ue(r);
    }
    r->err = 1;
}

static void skip_weights(Rd *r, int n, int chroma)
{
    for (int i = 0; i < n && !r->err; i++) {
        if (rd_bit(r)) { /* luma_weight_flag */
            rd_se(r);
            rd_se(r);
        }
        if (chroma && rd_bit(r)) /* chroma_weight_flag */
            for (int j = 0; j < 4; j++)
                rd_se(r);
    }
}

/* One slice NAL (header byte included) without its weight table, into out.
 * Returns the new length, 0 if it has none to lose (copy it as it is), -1 on error. */
static int strip_slice(const H264Strip *s, const uint8_t *nal, int n, uint8_t *out, int cap)
{
    int type = nal[0] & 0x1F, ref_idc = nal[0] >> 5 & 3, idr = type == 5;
    uint8_t *b = malloc(n + 8), *o = malloc(n + 16);
    if (!b || !o) {
        free(b);
        free(o);
        return -1;
    }
    int len = to_rbsp(nal + 1, n - 1, b, n + 8);
    int ret = -1;
    Rd r = {b, len < 0 ? 0 : len, 0, 0};
    rd_ue(&r); /* first_mb_in_slice */
    int st = (int)(rd_ue(&r) % 5);
    rd_ue(&r); /* pps id (one PPS is assumed: the one the MP4 gives) */
    int needs = (s->weighted_pred && (st == SL_P || st == SL_SP)) || (s->weighted_bipred == 1 && st == SL_B);
    if (!needs || r.err) {
        ret = r.err ? -1 : 0;
        goto out;
    }
    if (s->separate_colour_plane) rd_bits(&r, 2);
    rd_bits(&r, s->log2_max_frame_num);
    int field = 0;
    if (!s->frame_mbs_only && (field = rd_bit(&r)))
        rd_bit(&r); /* bottom_field_flag */
    if (idr) rd_ue(&r); /* idr_pic_id */
    if (s->poc_type == 0) {
        rd_bits(&r, s->log2_max_poc_lsb);
        if (s->bottom_field_poc && !field) rd_se(&r);
    } else if (s->poc_type == 1 && !s->delta_poc_always_zero) {
        rd_se(&r);
        if (s->bottom_field_poc && !field) rd_se(&r);
    }
    if (s->redundant_pic_cnt) rd_ue(&r);
    if (st == SL_B) rd_bit(&r); /* direct_spatial_mv_pred */
    int l0 = s->num_ref_l0, l1 = s->num_ref_l1;
    if (rd_bit(&r)) { /* num_ref_idx_active_override */
        l0 = (int)rd_ue(&r) + 1;
        if (st == SL_B) l1 = (int)rd_ue(&r) + 1;
    }
    if (l0 > 32 || l1 > 32) goto out;
    skip_ref_list_mod(&r);
    if (st == SL_B) skip_ref_list_mod(&r);
    long pwt_start = r.pos;
    rd_ue(&r); /* luma_log2_weight_denom */
    if (s->chroma_array_type) rd_ue(&r);
    skip_weights(&r, l0, s->chroma_array_type != 0);
    if (st == SL_B) skip_weights(&r, l1, s->chroma_array_type != 0);
    long pwt_end = r.pos;
    /* The rest of the header, to find where the slice data starts. */
    if (ref_idc) {
        if (idr) {
            rd_bit(&r);
            rd_bit(&r);
        } else if (rd_bit(&r)) { /* adaptive_ref_pic_marking */
            int k = 0;
            for (; k < 100 && !r.err; k++) {
                uint32_t mmco = rd_ue(&r);
                if (mmco == 0) break;
                if (mmco > 6) { r.err = 1; break; }
                if (mmco == 1 || mmco == 3) rd_ue(&r);
                if (mmco == 2) rd_ue(&r);
                if (mmco == 3 || mmco == 6) rd_ue(&r);
                if (mmco == 4) rd_ue(&r);
            }
            if (k == 100) r.err = 1;
        }
    }
    if (s->cabac && st != SL_I && st != SL_SI) rd_ue(&r); /* cabac_init_idc */
    rd_se(&r); /* slice_qp_delta */
    if (st == SL_SP || st == SL_SI) {
        if (st == SL_SP) rd_bit(&r);
        rd_se(&r);
    }
    if (s->deblocking_control && rd_ue(&r) != 1) {
        rd_se(&r);
        rd_se(&r);
    }
    long hdr_end = r.pos;
    if (r.err) goto out;
    /* the last 1 bit of the RBSP: rbsp_stop_one_bit (CAVLC) or the end of the CABAC data */
    long stop = (long)len * 8 - 1;
    while (stop >= 0 && !(b[stop >> 3] >> (7 - (stop & 7)) & 1))
        stop--;
    if (stop < hdr_end) goto out;

    Wr w = {o, n + 16, 0, 0};
    memset(o, 0, n + 16);
    copy_bits(&w, &r, 0, pwt_start);
    copy_bits(&w, &r, pwt_end, hdr_end);
    if (s->cabac) {
        /* cabac_alignment_one_bits, then the CABAC bytes as they were */
        long data = (hdr_end + 7) & ~7L;
        while ((w.pos & 7) && !w.err)
            wr_bit(&w, 1);
        if (w.err || data > (long)len * 8) goto out;
        int rest = len - (int)(data >> 3);
        if ((w.pos >> 3) + rest > n + 16) goto out;
        memcpy(o + (w.pos >> 3), b + (data >> 3), rest);
        w.pos += (long)rest * 8;
    } else {
        copy_bits(&w, &r, hdr_end, stop + 1); /* the macroblocks and the stop bit */
        while ((w.pos & 7) && !w.err)
            wr_bit(&w, 0);
    }
    if (w.err || cap < 1) goto out;
    out[0] = nal[0];
    int k = to_nal(o, (int)(w.pos >> 3), out + 1, cap - 1);
    ret = k < 0 ? -1 : k + 1;
out:
    free(b);
    free(o);
    return ret;
}

int h264_strip_sample(const H264Strip *s, const uint8_t *in, uint32_t n, int nal_length, uint8_t *out, uint32_t cap)
{
    if (!s->ok || nal_length < 1 || nal_length > 4)
        return -1;
    uint32_t pos = 0, k = 0;
    while (pos + (uint32_t)nal_length <= n) {
        uint32_t len = 0;
        for (int i = 0; i < nal_length; i++)
            len = len << 8 | in[pos + i];
        pos += nal_length;
        if (len == 0 || pos + len > n || len > 0x7FFFFFFF)
            return -1;
        const uint8_t *nal = in + pos;
        int type = nal[0] & 0x1F;
        if (k + nal_length > cap)
            return -1;
        uint8_t *dst = out + k + nal_length;
        int room = (int)(cap - k - nal_length), m = 0;
        if (type == 1 || type == 5)
            m = strip_slice(s, nal, (int)len, dst, room);
        else if (type == 8)
            m = h264_strip_pps(nal, (int)len, dst, room);
        if (m < 0)
            return -1;
        if (m == 0) { /* as it was */
            if (len > (uint32_t)room)
                return -1;
            memcpy(dst, nal, len);
            m = (int)len;
        }
        for (int i = 0; i < nal_length; i++)
            out[k + i] = (uint8_t)((uint32_t)m >> (8 * (nal_length - 1 - i)));
        k += nal_length + m;
        pos += len;
    }
    return pos == n ? (int)k : -1;
}
