/* XLT - MP4 (ISO BMFF) demuxer for PSP videos: one H.264 and one AAC track.
 * Portable (host-tested against ffprobe). Only the sample tables are kept in
 * memory; samples are read from the file when they are played. */
#ifndef MP4_H
#define MP4_H

#include <stdint.h>
#include "id3.h"

typedef struct {
    int present;
    uint32_t timescale;
    uint32_t count;           /* samples */
    uint32_t fixed_size;      /* stsz sample_size, 0 if per-sample */
    uint32_t *sizes;
    uint32_t nchunks;
    uint64_t *chunk_off;
    uint32_t *chunk_first;    /* first sample index of each chunk */
    uint32_t nstts;
    uint32_t (*stts)[2];      /* {count, delta} */
    uint32_t nsync;
    uint32_t *sync;           /* 0-based key-frame sample indices, NULL = every sample */
} Mp4Track;

/* One AAC audio track of the file (all are listed; only the chosen one is loaded). */
#define MP4_AUDIO_MAX 8
typedef struct {
    char lang[4];             /* ISO 639-2 from mdhd, e.g. "eng"; "" if undetermined */
    char name[40];            /* the track's title (udta name, or hdlr name), "" if none */
    int sample_rate, channels, aac_object;
    int enabled;              /* tkhd's enabled flag: the file's default */
} Mp4AudioInfo;

/* One text subtitle track (tx3g / mov_text, or QuickTime text); chapter tracks aren't listed. */
#define MP4_SUB_MAX 8
#define MP4_SUB_SAMPLE_MAX 2048          /* bytes of one cue read at most */
#define MP4_SUB_TEXT_MAX (2 * 1024 * 1024) /* a track's SubRip text at most */
typedef struct {
    char lang[4];             /* ISO 639-2, "" if undetermined */
    char name[40];            /* the track's title, "" if none */
    int enabled;              /* tkhd's enabled flag: the file's default */
} Mp4SubInfo;

typedef struct {
    Mp4Track video, audio;
    uint32_t duration_ms;
    /* video (avc1 / avcC) */
    int width, height;
    int profile, level;       /* e.g. 66 = Baseline */
    int nal_length;           /* bytes in front of each NAL (1, 2 or 4) */
    uint8_t sps[256], pps[256];
    int sps_size, pps_size;
    /* audio (mp4a / esds) */
    int sample_rate, channels;
    int aac_object;           /* 2 = AAC LC */
    /* every AAC track; `audio` (and the three fields above) is audios[audio_sel] */
    int naudio, audio_sel;
    Mp4AudioInfo audios[MP4_AUDIO_MAX];
    /* the text subtitle tracks, listed only: mp4_sub_texts() reads them */
    int nsub;
    Mp4SubInfo subs[MP4_SUB_MAX];
} Mp4;

/* From a PPS NAL (with its header byte): weighted_pred_flag (P slices) and
 * weighted_bipred_idc (B slices). Returns 0 if it could be read. */
int h264_pps_weights(const uint8_t *pps, int size, int *weighted_pred, int *weighted_bipred);
/* max_num_ref_frames from an SPS NAL (with its header byte), -1 if unreadable. */
int h264_sps_refs(const uint8_t *sps, int size);
/* 1 if a sample (NALs with nal_length-byte size prefixes) contains an IDR slice. */
int h264_sample_has_idr(const uint8_t *sample, uint32_t size, int nal_length);

/* Why the PSP's decoder can't play this video's H.264, in one plain sentence
 * written into why; 0 if nothing is known against it. Checks what the
 * firmware is known to refuse: weighted prediction (unless XLT took the
 * weights out: weights_stripped), more than 3 reference
 * frames, High profile, levels above 3.0, pictures larger than 720x480. */
int h264_psp_problem(const Mp4 *m, int weights_stripped, char *why, int n);
/* How to make a video the PSP plays (one sentence, for the error screen). */
extern const char H264_PSP_FIX[];

/* Parses the file's moov box. Returns 0 if it has a playable H.264 track
 * (audio is optional). On failure *why is set to a short reason. */
int mp4_open(Mp4 *m, const char *path, const char **why);
/* The same with a chosen audio track: its index in audios, or -1 for the one in
 * the language lang (ISO 639-2, may be NULL), else the file's default. */
int mp4_open_track(Mp4 *m, const char *path, const char **why, int audio, const char *lang);
/* "English" for "eng" (common languages), else the code itself; "" -> "". */
const char *mp4_lang_name(const char *code);
/* The same for music (.m4a): needs an AAC track, video is ignored if there is one. */
int mp4_open_audio(Mp4 *m, const char *path, const char **why);
void mp4_close(Mp4 *m);
/* Reads text subtitle tracks as SubRip text, for srt_parse: order[] lists them
 * (indices into m->subs) in the order wanted. done() gets each one as it's
 * finished: the text is malloc'd and its own (NULL if the track has none).
 * One small read per cue, so it belongs on a worker; stop (may be NULL) ends it
 * early, without calling done() for the track cut short. Returns how many were done. */
typedef void (*Mp4SubDone)(int track, char *text, int len, void *ctx);
int mp4_sub_read(const char *path, const int *order, int n, Mp4SubDone done, void *ctx, volatile int *stop);

/* iTunes-style tags. The cover is only located. 0 if the file could be read. */
int mp4_tags(const char *path, Id3Info *out);
/* The lyrics tag (\xA9lyr) as UTF-8. 0 if there are some. */
int mp4_lyrics(const char *path, char *out, int cap);

/* Byte offset and size of sample i of track t. */
uint64_t mp4_offset(const Mp4Track *t, uint32_t i);
uint32_t mp4_size(const Mp4Track *t, uint32_t i);
/* Decode time of sample i in ms, and the sample at (or just before) ms. */
uint32_t mp4_time_ms(const Mp4Track *t, uint32_t i);
uint32_t mp4_sample_at(const Mp4Track *t, uint32_t ms);
/* The chunk holding sample i: its first sample and how many samples it has. */
void mp4_chunk_of(const Mp4Track *t, uint32_t i, uint32_t *first, uint32_t *count);
/* The key frame at or before sample i. */
uint32_t mp4_sync_before(const Mp4Track *t, uint32_t i);
/* The key frame at or after sample i; t->count if there is none. */
uint32_t mp4_sync_after(const Mp4Track *t, uint32_t i);
int mp4_is_sync(const Mp4Track *t, uint32_t i);

#endif
