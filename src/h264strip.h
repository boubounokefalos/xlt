/* XLT - H.264 weighted prediction stripper. Portable (host-tested).
 *
 * The PSP's decoder refuses streams with explicit weighted prediction (x264's
 * default weightp: weighted_pred_flag = 1 in the PPS, a pred_weight_table in
 * every P slice header). This rewrites them as the decoder will take them, as
 * PMPlayer's compatibility path does: the PPS flag goes to 0 and each P slice
 * (and B slice, for the rare weighted_bipred_idc = 1) loses its weight table;
 * the rest of the slice is kept bit for bit (CABAC data stays byte-aligned).
 *
 * The cost: a block predicted from a weighted reference is now predicted
 * without the weight, so fades (where x264 uses weights) look a little off
 * until the next key frame. Pictures without weights decode exactly as before.
 */
#ifndef H264STRIP_H
#define H264STRIP_H

#include <stdint.h>

typedef struct {
    /* SPS */
    int log2_max_frame_num, poc_type, log2_max_poc_lsb, delta_poc_always_zero;
    int frame_mbs_only, chroma_array_type, separate_colour_plane;
    /* PPS */
    int cabac, bottom_field_poc, num_ref_l0, num_ref_l1;
    int weighted_pred, weighted_bipred, redundant_pic_cnt, deblocking_control;
    int ok; /* both parsed */
} H264Strip;

/* Reads the SPS and PPS (NALs with their header byte, no start code or size).
 * Returns 1 if the stream needs stripping, 0 if not, -1 if they can't be read. */
int h264_strip_init(H264Strip *s, const uint8_t *sps, int sps_n, const uint8_t *pps, int pps_n);

/* The PPS with weighted prediction off (same length or longer by emulation
 * bytes). Returns the new size, -1 on failure. cap >= pps_n + 8 is enough. */
int h264_strip_pps(const uint8_t *pps, int n, uint8_t *out, int cap);

/* Rewrites one MP4 sample (NALs with nal_length-byte big-endian sizes) into out.
 * Slices lose their weight tables; a PPS inside the sample is rewritten too;
 * everything else is copied. Returns the new size, or -1 (a slice it can't
 * read, or out too small: then play the sample as it was). cap >= n + n / 4 + 64
 * is always enough. */
int h264_strip_sample(const H264Strip *s, const uint8_t *in, uint32_t n, int nal_length, uint8_t *out, uint32_t cap);

#endif
