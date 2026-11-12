/* XLT - MP4 demuxer. See mp4.h.
 *
 * Box layout used: moov > trak > mdia > {mdhd, hdlr, minf > stbl > {stsd,
 * stts, stss, stsc, stsz, stco|co64}}. The whole moov box is read into memory
 * once (it holds only tables), parsed, and freed. Fragmented MP4 (moof) and
 * edit lists are not supported; PSP-encoded files don't use them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mp4.h"
#include "xio.h"

#define MAX_MOOV (16 * 1024 * 1024)

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

typedef struct {
    const uint8_t *p; /* payload */
    uint64_t n;       /* payload size */
    char type[5];
} Box;

/* Reads the box at buf[*pos]; advances *pos. 0 on success. */
static int next_box(const uint8_t *buf, uint64_t len, uint64_t *pos, Box *b)
{
    if (*pos + 8 > len)
        return -1;
    const uint8_t *h = buf + *pos;
    uint64_t size = be32(h), hdr = 8;
    if (size == 1) {
        if (*pos + 16 > len) return -1;
        size = be64(h + 8);
        hdr = 16;
    } else if (size == 0) {
        size = len - *pos;
    }
    if (size < hdr || *pos + size > len)
        return -1;
    memcpy(b->type, h + 4, 4);
    b->type[4] = 0;
    b->p = h + hdr;
    b->n = size - hdr;
    *pos += size;
    return 0;
}

/* Finds the first child box of `type` inside the container payload. */
static int child(const Box *parent, const char *type, uint64_t skip, Box *out)
{
    uint64_t pos = skip;
    Box b;
    while (next_box(parent->p, parent->n, &pos, &b) == 0)
        if (!memcmp(b.type, type, 4)) {
            *out = b;
            return 0;
        }
    return -1;
}

/* ------------------------------------------------------------ sample tables */

static int parse_tables(const Box *stbl, Mp4Track *t)
{
    Box b;
    /* stsz */
    if (child(stbl, "stsz", 0, &b) || b.n < 12) return -1;
    t->fixed_size = be32(b.p + 4);
    t->count = be32(b.p + 8);
    if (t->count == 0 || t->count > 4000000) return -1;
    if (!t->fixed_size) {
        if (b.n < 12 + (uint64_t)t->count * 4) return -1;
        t->sizes = malloc(sizeof(uint32_t) * t->count);
        if (!t->sizes) return -1;
        for (uint32_t i = 0; i < t->count; i++) t->sizes[i] = be32(b.p + 12 + i * 4);
    }
    /* chunk offsets */
    int co64 = 0;
    if (child(stbl, "stco", 0, &b)) {
        if (child(stbl, "co64", 0, &b)) return -1;
        co64 = 1;
    }
    if (b.n < 8) return -1;
    t->nchunks = be32(b.p + 4);
    if (t->nchunks == 0 || b.n < 8 + (uint64_t)t->nchunks * (co64 ? 8 : 4)) return -1;
    t->chunk_off = malloc(sizeof(uint64_t) * t->nchunks);
    t->chunk_first = malloc(sizeof(uint32_t) * t->nchunks);
    if (!t->chunk_off || !t->chunk_first) return -1;
    for (uint32_t i = 0; i < t->nchunks; i++)
        t->chunk_off[i] = co64 ? be64(b.p + 8 + i * 8) : be32(b.p + 8 + i * 4);
    /* stsc: samples per chunk, as runs */
    if (child(stbl, "stsc", 0, &b) || b.n < 8) return -1;
    uint32_t ne = be32(b.p + 4);
    if (ne == 0 || b.n < 8 + (uint64_t)ne * 12) return -1;
    uint32_t s = 0;
    for (uint32_t e = 0; e < ne; e++) {
        uint32_t first = be32(b.p + 8 + e * 12) - 1, spc = be32(b.p + 8 + e * 12 + 4);
        uint32_t last = e + 1 < ne ? be32(b.p + 8 + (e + 1) * 12) - 1 : t->nchunks;
        if (first >= t->nchunks || last > t->nchunks || last < first) return -1;
        for (uint32_t c = first; c < last; c++) {
            t->chunk_first[c] = s;
            s += spc;
        }
    }
    if (s < t->count) return -1;
    /* stts */
    if (child(stbl, "stts", 0, &b) || b.n < 8) return -1;
    t->nstts = be32(b.p + 4);
    if (t->nstts == 0 || b.n < 8 + (uint64_t)t->nstts * 8) return -1;
    t->stts = malloc(sizeof(uint32_t) * 2 * t->nstts);
    if (!t->stts) return -1;
    for (uint32_t i = 0; i < t->nstts; i++) {
        t->stts[i][0] = be32(b.p + 8 + i * 8);
        t->stts[i][1] = be32(b.p + 12 + i * 8);
    }
    /* stss (optional: absent = every sample is a key frame) */
    if (child(stbl, "stss", 0, &b) == 0 && b.n >= 8) {
        t->nsync = be32(b.p + 4);
        if (b.n < 8 + (uint64_t)t->nsync * 4) return -1;
        t->sync = malloc(sizeof(uint32_t) * (t->nsync ? t->nsync : 1));
        if (!t->sync) return -1;
        for (uint32_t i = 0; i < t->nsync; i++) t->sync[i] = be32(b.p + 8 + i * 4) - 1;
    }
    t->present = 1;
    return 0;
}

/* ------------------------------------------------------- sample descriptions */

static int parse_avc1(const Box *stsd, Mp4 *m)
{
    /* stsd: version/flags, entry count, then the first entry box */
    if (stsd->n < 8) return -1;
    uint64_t pos = 8;
    Box e;
    if (next_box(stsd->p, stsd->n, &pos, &e) || memcmp(e.type, "avc1", 4) || e.n < 78) return -1;
    m->width = be16(e.p + 24);
    m->height = be16(e.p + 26);
    Box c;
    if (child(&e, "avcC", 78, &c) || c.n < 7) return -1;
    const uint8_t *p = c.p;
    m->profile = p[1];
    m->level = p[3];
    m->nal_length = (p[4] & 3) + 1;
    uint64_t at = 5;
    int nsps = p[at++] & 31;
    for (int i = 0; i < nsps; i++) {
        if (at + 2 > c.n) return -1;
        uint32_t n = be16(p + at);
        at += 2;
        if (at + n > c.n) return -1;
        if (i == 0 && n <= sizeof(m->sps)) { memcpy(m->sps, p + at, n); m->sps_size = n; }
        at += n;
    }
    if (at + 1 > c.n) return -1;
    int npps = p[at++];
    for (int i = 0; i < npps; i++) {
        if (at + 2 > c.n) return -1;
        uint32_t n = be16(p + at);
        at += 2;
        if (at + n > c.n) return -1;
        if (i == 0 && n <= sizeof(m->pps)) { memcpy(m->pps, p + at, n); m->pps_size = n; }
        at += n;
    }
    return m->sps_size && m->pps_size ? 0 : -1;
}

/* Reads an MPEG-4 descriptor length (1-4 bytes of 7 bits). */
static uint32_t desc_len(const uint8_t *p, uint64_t n, uint64_t *at)
{
    uint32_t len = 0;
    for (int i = 0; i < 4 && *at < n; i++) {
        uint8_t b = p[(*at)++];
        len = len << 7 | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return len;
}

static int parse_mp4a(const Box *stsd, Mp4 *m)
{
    if (stsd->n < 8) return -1;
    uint64_t pos = 8;
    Box e;
    if (next_box(stsd->p, stsd->n, &pos, &e) || memcmp(e.type, "mp4a", 4) || e.n < 28) return -1;
    m->channels = be16(e.p + 16);
    m->sample_rate = be32(e.p + 24) >> 16;
    m->aac_object = 2;
    Box es;
    if (child(&e, "esds", 28, &es) == 0 && es.n > 4) {
        /* ES_Descriptor (3) > DecoderConfigDescriptor (4) > DecoderSpecificInfo (5) */
        const uint8_t *p = es.p;
        uint64_t at = 4;
        while (at + 2 <= es.n) {
            uint8_t tag = p[at++];
            uint32_t len = desc_len(p, es.n, &at);
            if (tag == 3) { at += 3; continue; }       /* ES_ID + flags, then children */
            if (tag == 4) { at += 13; continue; }      /* objectType ... avgBitrate, then children */
            if (tag == 5 && len >= 2 && at + 2 <= es.n) {
                int obj = p[at] >> 3, sri = (p[at] & 7) << 1 | p[at + 1] >> 7;
                static const int rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                              22050, 16000, 12000, 11025, 8000, 7350};
                m->aac_object = obj;
                if (sri < 13) m->sample_rate = rates[sri];
                m->channels = (p[at + 1] >> 3) & 15;
                break;
            }
            at += len;
        }
    }
    return m->sample_rate > 0 && m->channels > 0 ? 0 : -1;
}

static void free_track(Mp4Track *t);

/* A track's title: udta > name, else the hdlr name unless it's a muxer's generic one. */
static void track_name(const Box *trak, const Box *hdlr, char *out, int n)
{
    out[0] = 0;
    Box udta, name;
    if (child(trak, "udta", 0, &udta) == 0 && child(&udta, "name", 0, &name) == 0 && name.n > 0) {
        snprintf(out, n, "%.*s", (int)(name.n < (uint64_t)n ? name.n : (uint64_t)n - 1), (const char *)name.p);
    } else if (hdlr->n > 24) {
        const char *s = (const char *)hdlr->p + 24;
        int len = (int)(hdlr->n - 24);
        if (len > 0 && (uint8_t)s[0] == len - 1) { s++; len--; } /* a Pascal string (QuickTime) */
        while (len > 0 && !s[len - 1]) len--;
        snprintf(out, n, "%.*s", len < n ? len : n - 1, s);
        if (!strcmp(out, "SoundHandler") || !strncmp(out, "Core Media", 10) || !strcmp(out, "Sound Media Handler") ||
            !strncmp(out, "ISO Media", 9) || !strncmp(out, "GPAC", 4) || !strncmp(out, "L-SMASH ", 8) ||
            !strcmp(out, "SubtitleHandler") || !strcmp(out, "TextHandler") || !strcmp(out, "Subtitle Media Handler") ||
            !strcmp(out, "Text Media Handler"))
            out[0] = 0;
    }
    for (char *c = out; *c; c++)
        if ((unsigned char)*c < 0x20) *c = ' ';
}

/* ---------------------------------------------------------- text subtitles */

static uint8_t *load_moov(const char *path, uint64_t *size, uint64_t *at, int *moof, const char **why);

/* The IDs of chapter-title tracks (tref > chap of any track): text tracks, but not subtitles. */
static int chapter_ids(const Box *moov, uint32_t *ids, int max)
{
    int n = 0;
    uint64_t q = 0;
    Box t, tref, chap;
    while (next_box(moov->p, moov->n, &q, &t) == 0)
        if (!memcmp(t.type, "trak", 4) && child(&t, "tref", 0, &tref) == 0 && child(&tref, "chap", 0, &chap) == 0)
            for (uint64_t i = 0; i + 4 <= chap.n && n < max; i += 4)
                ids[n++] = be32(chap.p + i);
    return n;
}

/* A subtitle track: hdlr sbtl or text, sample entry tx3g or text, not a chapter track.
 * Gives its stbl and timescale. */
static int sub_trak(const Box *trak, const uint32_t *chap, int nchap, Box *stbl, uint32_t *timescale,
                    Box *hdlr_out, Box *mdhd_out)
{
    Box mdia, hdlr, mdhd, minf, stsd, e, tkhd;
    if (child(trak, "mdia", 0, &mdia) || child(&mdia, "hdlr", 0, &hdlr) || hdlr.n < 12 ||
        (memcmp(hdlr.p + 8, "sbtl", 4) && memcmp(hdlr.p + 8, "text", 4)) ||
        child(&mdia, "mdhd", 0, &mdhd) || mdhd.n < 24 || child(&mdia, "minf", 0, &minf) ||
        child(&minf, "stbl", 0, stbl) || child(stbl, "stsd", 0, &stsd) || stsd.n < 8)
        return 0;
    uint64_t pos = 8;
    if (next_box(stsd.p, stsd.n, &pos, &e) || (memcmp(e.type, "tx3g", 4) && memcmp(e.type, "text", 4)))
        return 0;
    if (child(trak, "tkhd", 0, &tkhd) == 0 && tkhd.n >= 24) {
        uint32_t id = be32(tkhd.p + (tkhd.p[0] == 1 ? 20 : 12));
        for (int i = 0; i < nchap; i++)
            if (chap[i] == id) return 0;
    }
    *timescale = mdhd.p[0] == 1 ? be32(mdhd.p + 20) : be32(mdhd.p + 12);
    if (hdlr_out) *hdlr_out = hdlr;
    if (mdhd_out) *mdhd_out = mdhd;
    return *timescale != 0;
}

/* ISO 639-2 from mdhd, "" if undetermined. */
static void mdhd_lang(const Box *mdhd, char out[4])
{
    out[0] = 0;
    uint64_t lo = mdhd->p[0] == 1 ? 32 : 20;
    if (mdhd->n < lo + 2) return;
    unsigned l = be16(mdhd->p + lo);
    char c[4] = {(char)(((l >> 10) & 31) + 0x60), (char)(((l >> 5) & 31) + 0x60), (char)((l & 31) + 0x60), 0};
    if (l && strcmp(c, "und") && c[0] >= 'a' && c[0] <= 'z' && c[1] >= 'a' && c[1] <= 'z' && c[2] >= 'a' && c[2] <= 'z')
        memcpy(out, c, 4);
}

static void list_subs(const Box *moov, Mp4 *m)
{
    uint32_t chap[16];
    int nchap = chapter_ids(moov, chap, 16);
    uint64_t q = 0;
    Box t, stbl, hdlr, mdhd, tkhd;
    uint32_t ts;
    while (next_box(moov->p, moov->n, &q, &t) == 0 && m->nsub < MP4_SUB_MAX) {
        if (memcmp(t.type, "trak", 4) || !sub_trak(&t, chap, nchap, &stbl, &ts, &hdlr, &mdhd))
            continue;
        Mp4SubInfo *s = &m->subs[m->nsub++];
        memset(s, 0, sizeof(*s));
        mdhd_lang(&mdhd, s->lang);
        track_name(&t, &hdlr, s->name, sizeof(s->name));
        s->enabled = child(&t, "tkhd", 0, &tkhd) != 0 || tkhd.n < 4 || (tkhd.p[3] & 1);
    }
}

typedef struct {
    char *p;
    int n, cap;
} Text;

static int text_add(Text *t, const char *s, int n)
{
    if (t->n + n + 1 > t->cap) {
        int cap = t->cap ? t->cap * 2 : 4096;
        while (cap < t->n + n + 1) cap *= 2;
        if (cap > MP4_SUB_TEXT_MAX) return -1;
        char *p = realloc(t->p, cap);
        if (!p) return -1;
        t->p = p;
        t->cap = cap;
    }
    memcpy(t->p + t->n, s, n);
    t->n += n;
    t->p[t->n] = 0;
    return 0;
}

static void srt_time(char *out, uint32_t t)
{
    unsigned ms = (unsigned)t;
    snprintf(out, 16, "%02u:%02u:%02u,%03u", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
}

/* One sample's text as SubRip cue lines: UTF-16 (with its mark) made UTF-8, CR
 * dropped, no blank lines (they would end the cue). Returns its length in out. */
static int cue_text(const uint8_t *p, int n, char *out, int cap)
{
    int o = 0;
    char last = '\n';
    int u16 = n >= 2 && p[0] == 0xFE && p[1] == 0xFF;
    for (int i = u16 ? 2 : 0; i < n && o < cap - 4;) {
        unsigned c;
        if (u16) {
            if (i + 1 >= n) break;
            c = be16(p + i);
            i += 2;
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
                unsigned d = be16(p + i);
                if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i += 2; }
            }
        } else {
            c = p[i++];
        }
        if (c == '\r' || c == 0) continue;
        if (c == '\n' && last == '\n') continue;
        if (u16 && c >= 0x80) { /* UTF-8 bytes out */
            if (c < 0x800) { out[o++] = (char)(0xC0 | c >> 6); }
            else if (c < 0x10000) { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | (c >> 6 & 63)); }
            else { out[o++] = (char)(0xF0 | c >> 18); out[o++] = (char)(0x80 | (c >> 12 & 63)); out[o++] = (char)(0x80 | (c >> 6 & 63)); }
            out[o++] = (char)(0x80 | (c & 63));
            last = 0;
        } else {
            out[o++] = (char)c;
            last = (char)c;
        }
    }
    while (o > 0 && out[o - 1] == '\n') o--;
    return o;
}

/* Track t's cues as SubRip text (malloc'd into *out, NULL if none). Reads one
 * small piece of the file per cue; stop is checked between them. */
static void read_sub_track(int fd, const Mp4Track *tr, uint32_t ts, volatile int *stop, char **out, int *len)
{
    static uint8_t sample[MP4_SUB_SAMPLE_MAX];
    static char cue[MP4_SUB_SAMPLE_MAX * 2];
    Text text = {0};
    /* the samples in file order: chunk by chunk, with their times from stts */
    uint32_t e = 0, left = tr->nstts ? tr->stts[0][0] : 0, i = 0, cues = 0;
    uint64_t ticks = 0;
    int full = 0;
    for (uint32_t c = 0; c < tr->nchunks && i < tr->count && !full; c++) {
        uint32_t last = c + 1 < tr->nchunks ? tr->chunk_first[c + 1] : tr->count;
        if (last > tr->count) last = tr->count;
        uint64_t off = tr->chunk_off[c];
        for (; i < last && !full; i++) {
            if (stop && *stop) { full = 1; break; }
            while (!left && e + 1 < tr->nstts) left = tr->stts[++e][0];
            uint32_t dur = e < tr->nstts ? tr->stts[e][1] : 0;
            if (left) left--;
            uint32_t size = tr->fixed_size ? tr->fixed_size : tr->sizes[i];
            uint32_t a = (uint32_t)(ticks * 1000 / ts), b = (uint32_t)((ticks + dur) * 1000 / ts);
            ticks += dur;
            uint32_t got = size < sizeof(sample) ? size : sizeof(sample);
            if (size >= 3 && b > a && xio_read_at(fd, sample, got, off) == (int)got) {
                uint32_t tl = be16(sample);
                if (tl > got - 2) tl = got - 2;
                int cl = cue_text(sample + 2, (int)tl, cue, sizeof(cue));
                if (cl > 0) {
                    char head[64], ta[16], tb[16];
                    srt_time(ta, a);
                    srt_time(tb, b);
                    int hl = snprintf(head, sizeof(head), "%u\n%s --> %s\n", (unsigned)++cues, ta, tb);
                    full = text_add(&text, head, hl) || text_add(&text, cue, cl) ||
                           text_add(&text, "\n\n", 2); /* too much: what's there is kept */
                }
            }
            off += size;
        }
    }
    *out = text.p;
    *len = text.n;
}

int mp4_sub_read(const char *path, const int *order, int n, Mp4SubDone done, void *ctx, volatile int *stop)
{
    if (n > MP4_SUB_MAX) n = MP4_SUB_MAX;
    const char *why;
    uint64_t moov_size, moov_at;
    int moof;
    uint8_t *buf = load_moov(path, &moov_size, &moov_at, &moof, &why);
    if (!buf)
        return 0;
    /* the tables of the tracks asked for, then the index can go: a film's is megabytes */
    Mp4Track tr[MP4_SUB_MAX];
    uint32_t ts[MP4_SUB_MAX];
    memset(tr, 0, sizeof(tr));
    memset(ts, 0, sizeof(ts));
    uint64_t p0 = 0;
    Box moov, t, stbl;
    if (next_box(buf, moov_size, &p0, &moov) == 0) {
        uint32_t chap[16];
        int nchap = chapter_ids(&moov, chap, 16), k = 0;
        uint64_t q = 0;
        while (next_box(moov.p, moov.n, &q, &t) == 0 && k < MP4_SUB_MAX) {
            uint32_t sts;
            if (memcmp(t.type, "trak", 4) || !sub_trak(&t, chap, nchap, &stbl, &sts, NULL, NULL))
                continue;
            for (int i = 0; i < n; i++)
                if (order[i] == k && !tr[i].present && parse_tables(&stbl, &tr[i]) == 0)
                    ts[i] = sts;
            k++;
        }
    }
    free(buf);
    int fd = xio_open(path), did = 0;
    for (int i = 0; i < n; i++) {
        if (fd >= 0 && tr[i].present && !(stop && *stop)) {
            char *text = NULL;
            int len = 0;
            read_sub_track(fd, &tr[i], ts[i], stop, &text, &len);
            if (stop && *stop) {
                free(text);
            } else {
                done(order[i], text, len, ctx);
                did++;
            }
        }
        free_track(&tr[i]);
    }
    if (fd >= 0)
        xio_close(fd);
    return did;
}

/* audio: the index of the AAC track to load (in the order they're met), -2 = list only. */
static void parse_trak(const Box *trak, Mp4 *m, int audio)
{
    Box mdia, hdlr, mdhd, minf, stbl, stsd;
    if (child(trak, "mdia", 0, &mdia) || child(&mdia, "hdlr", 0, &hdlr) || hdlr.n < 12 ||
        child(&mdia, "mdhd", 0, &mdhd) || mdhd.n < 24 || child(&mdia, "minf", 0, &minf) ||
        child(&minf, "stbl", 0, &stbl) || child(&stbl, "stsd", 0, &stsd))
        return;
    uint32_t timescale = mdhd.p[0] == 1 ? be32(mdhd.p + 20) : be32(mdhd.p + 12);
    if (!timescale) return;
    if (!memcmp(hdlr.p + 8, "vide", 4) && !m->video.present) {
        if (audio == -2) /* listing the audio tracks only */
            return;
        if (parse_avc1(&stsd, m) == 0 && parse_tables(&stbl, &m->video) == 0)
            m->video.timescale = timescale;
        else
            free_track(&m->video);
    } else if (!memcmp(hdlr.p + 8, "soun", 4) && m->naudio < MP4_AUDIO_MAX) {
        Mp4 a; /* the track's format, without touching m's until it's the one */
        memset(&a, 0, sizeof(a));
        if (parse_mp4a(&stsd, &a) != 0)
            return;
        Mp4AudioInfo *in = &m->audios[m->naudio];
        memset(in, 0, sizeof(*in));
        in->sample_rate = a.sample_rate;
        in->channels = a.channels;
        in->aac_object = a.aac_object;
        Box tkhd;
        in->enabled = child(trak, "tkhd", 0, &tkhd) != 0 || tkhd.n < 4 || (tkhd.p[3] & 1);
        uint64_t lo = mdhd.p[0] == 1 ? 32 : 20;
        if (mdhd.n >= lo + 2) {
            unsigned l = be16(mdhd.p + lo);
            char c[4] = {(char)(((l >> 10) & 31) + 0x60), (char)(((l >> 5) & 31) + 0x60), (char)((l & 31) + 0x60), 0};
            if (l && strcmp(c, "und") && c[0] >= 'a' && c[0] <= 'z' && c[1] >= 'a' && c[1] <= 'z' && c[2] >= 'a' && c[2] <= 'z')
                memcpy(in->lang, c, 4);
        }
        track_name(trak, &hdlr, in->name, sizeof(in->name));
        if (audio == m->naudio && !m->audio.present) {
            if (parse_tables(&stbl, &m->audio) == 0) {
                m->audio.timescale = timescale;
                m->sample_rate = a.sample_rate;
                m->channels = a.channels;
                m->aac_object = a.aac_object;
                m->audio_sel = m->naudio;
            } else {
                free_track(&m->audio);
            }
        }
        m->naudio++;
    }
}

/* The file's moov box in memory (malloc'd); *at is where it is in the file. NULL on failure. */
static uint8_t *load_moov(const char *path, uint64_t *size, uint64_t *at, int *moof, const char **why)
{
    int fd = xio_open(path);
    if (fd < 0) { *why = "can't open the file"; return NULL; }

    /* Walk the top-level boxes to find moov (it may be at the end). */
    uint64_t pos = 0, moov_at = 0, moov_size = 0;
    *moof = 0;
    for (int guard = 0; guard < 10000; guard++) {
        uint8_t h[16];
        if (xio_read_at(fd, h, 16, pos) < 8) break;
        uint64_t box = be32(h);
        if (box == 1) box = be64(h + 8);
        if (!memcmp(h + 4, "moof", 4)) *moof = 1;
        if (!memcmp(h + 4, "moov", 4)) { moov_at = pos; moov_size = box; }
        if (box < 8) break; /* size 0 = to the end of the file: nothing after it */
        pos += box;
    }
    if (!moov_size) { xio_close(fd); *why = *moof ? "fragmented MP4 isn't supported" : "not an MP4 file"; return NULL; }
    if (moov_size > MAX_MOOV) { xio_close(fd); *why = "MP4 index too large"; return NULL; }
    uint8_t *buf = malloc(moov_size);
    if (!buf || xio_read_at(fd, buf, (uint32_t)moov_size, moov_at) != (int)moov_size) {
        free(buf);
        xio_close(fd);
        *why = "can't read the MP4 index";
        return NULL;
    }
    xio_close(fd);
    *size = moov_size;
    *at = moov_at;
    return buf;
}

static int open_common(Mp4 *m, const char *path, const char **why, int audio_only, int audio, const char *lang)
{
    static const char *dummy;
    if (!why) why = &dummy;
    memset(m, 0, sizeof(*m));
    uint64_t moov_size, moov_at;
    int moof;
    uint8_t *buf = load_moov(path, &moov_size, &moov_at, &moof, why);
    if (!buf)
        return -1;
    uint64_t p = 0;
    Box moov, b;
    if (next_box(buf, moov_size, &p, &moov)) { free(buf); *why = "bad MP4 index"; return -1; }
    if (child(&moov, "mvhd", 0, &b) == 0 && b.n >= 20) {
        uint32_t ts = b.p[0] == 1 ? be32(b.p + 20) : be32(b.p + 12);
        uint64_t d = b.p[0] == 1 ? be64(b.p + 24) : be32(b.p + 16);
        m->duration_ms = ts ? (uint32_t)(d * 1000 / ts) : 0;
    }
    /* First the list of audio tracks, then the chosen one with its tables. */
    uint64_t q = 0;
    while (next_box(moov.p, moov.n, &q, &b) == 0)
        if (!memcmp(b.type, "trak", 4))
            parse_trak(&b, m, -2);
    list_subs(&moov, m);
    if (audio < 0 || audio >= m->naudio) {
        audio = -1;
        for (int i = 0; i < m->naudio && audio < 0 && lang && lang[0]; i++) /* the language asked for */
            if (!strcmp(m->audios[i].lang, lang)) audio = i;
        for (int i = 0; i < m->naudio && audio < 0; i++) /* else the file's default */
            if (m->audios[i].enabled) audio = i;
        if (audio < 0 && m->naudio) audio = 0;
    }
    m->naudio = 0; /* read again, with the chosen track's tables */
    q = 0;
    while (next_box(moov.p, moov.n, &q, &b) == 0)
        if (!memcmp(b.type, "trak", 4))
            parse_trak(&b, m, audio);
    free(buf);
    if (audio_only ? !m->audio.present : !m->video.present) {
        mp4_close(m);
        *why = moof ? "fragmented MP4 isn't supported" : audio_only ? "no AAC audio track" : "no H.264 video track";
        return -1;
    }
    return 0;
}

int mp4_open(Mp4 *m, const char *path, const char **why) { return open_common(m, path, why, 0, -1, NULL); }
int mp4_open_track(Mp4 *m, const char *path, const char **why, int audio, const char *lang)
{
    return open_common(m, path, why, 0, audio, lang);
}
int mp4_open_audio(Mp4 *m, const char *path, const char **why) { return open_common(m, path, why, 1, -1, NULL); }

const char *mp4_lang_name(const char *code)
{
    static const char *const names[][2] = {
        {"eng", "English"}, {"gre", "Greek"}, {"ell", "Greek"}, {"jpn", "Japanese"}, {"spa", "Spanish"},
        {"fre", "French"}, {"fra", "French"}, {"ger", "German"}, {"deu", "German"}, {"ita", "Italian"},
        {"por", "Portuguese"}, {"rus", "Russian"}, {"chi", "Chinese"}, {"zho", "Chinese"}, {"kor", "Korean"},
        {"dut", "Dutch"}, {"nld", "Dutch"}, {"swe", "Swedish"}, {"nor", "Norwegian"}, {"dan", "Danish"},
        {"fin", "Finnish"}, {"pol", "Polish"}, {"tur", "Turkish"}, {"ara", "Arabic"}, {"heb", "Hebrew"},
        {"hin", "Hindi"}, {"cze", "Czech"}, {"ces", "Czech"}, {"hun", "Hungarian"}, {"rum", "Romanian"},
        {"ron", "Romanian"}, {"bul", "Bulgarian"}, {"ukr", "Ukrainian"}, {"tha", "Thai"}, {"vie", "Vietnamese"},
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(code, names[i][0]))
            return names[i][1];
    return code;
}

/* ------------------------------------------------------------------- tags */

/* moov > udta > meta > ilst. 0 if it is there. */
static int find_ilst(const Box *moov, Box *ilst)
{
    Box udta, meta;
    if (child(moov, "udta", 0, &udta) || child(&udta, "meta", 0, &meta))
        return -1;
    return child(&meta, "ilst", 4, ilst) == 0 || child(&meta, "ilst", 0, ilst) == 0 ? 0 : -1;
}

/* The payload of an item's data box. 0 if it has one. */
static int item_data(const Box *item, const uint8_t **p, uint32_t *n)
{
    Box d;
    if (child(item, "data", 0, &d) || d.n < 8)
        return -1;
    *p = d.p + 8;
    *n = (uint32_t)(d.n - 8);
    return 0;
}

static void copy_text(char *out, int cap, const uint8_t *p, uint32_t n)
{
    uint32_t m = n < (uint32_t)cap - 1 ? n : (uint32_t)cap - 1;
    while (m > 0 && m < n && (p[m] & 0xC0) == 0x80) /* not in the middle of a letter */
        m--;
    memcpy(out, p, m);
    out[m] = 0;
}

int mp4_tags(const char *path, Id3Info *out)
{
    const char *why;
    uint64_t size, at, pos = 0, q = 0;
    int moof;
    memset(out, 0, sizeof(*out));
    uint8_t *buf = load_moov(path, &size, &at, &moof, &why);
    if (!buf)
        return -1;
    Box moov, ilst, item;
    if (next_box(buf, size, &pos, &moov) == 0 && find_ilst(&moov, &ilst) == 0) {
        char album_artist[sizeof(out->artist)] = "";
        while (next_box(ilst.p, ilst.n, &q, &item) == 0) {
            const uint8_t *p;
            uint32_t n;
            if (item_data(&item, &p, &n))
                continue;
            if (!memcmp(item.type, "\xA9nam", 4)) copy_text(out->title, sizeof(out->title), p, n);
            else if (!memcmp(item.type, "\xA9" "ART", 4)) copy_text(out->artist, sizeof(out->artist), p, n);
            else if (!memcmp(item.type, "aART", 4)) copy_text(album_artist, sizeof(album_artist), p, n);
            else if (!memcmp(item.type, "\xA9" "alb", 4)) copy_text(out->album, sizeof(out->album), p, n);
            else if (!memcmp(item.type, "trkn", 4) && n >= 4) out->track = (int)be16(p + 2);
            else if (!memcmp(item.type, "----", 4)) { /* freeform: mean com.apple.iTunes, name, data */
                Box nm;
                if (child(&item, "name", 0, &nm) == 0 && nm.n > 4 && nm.n < 40) {
                    char key[40], val[80];
                    snprintf(key, sizeof(key), "%.*s", (int)(nm.n - 4), (const char *)nm.p + 4);
                    snprintf(val, sizeof(val), "%.*s", (int)(n < sizeof(val) - 1 ? n : sizeof(val) - 1), (const char *)p);
                    id3_take_gain(out, key, val);
                }
            } else if (!memcmp(item.type, "covr", 4) && n && !out->art_size) {
                out->art_off = (uint32_t)(at + (uint64_t)(p - buf));
                out->art_size = n;
            }
        }
        if (!out->artist[0])
            memcpy(out->artist, album_artist, sizeof(album_artist));
    }
    free(buf);
    return 0;
}

int mp4_lyrics(const char *path, char *out, int cap)
{
    const char *why;
    uint64_t size, at, pos = 0, q = 0;
    int moof, found = 0;
    out[0] = 0;
    uint8_t *buf = load_moov(path, &size, &at, &moof, &why);
    if (!buf)
        return -1;
    Box moov, ilst, item;
    if (next_box(buf, size, &pos, &moov) == 0 && find_ilst(&moov, &ilst) == 0)
        while (!found && next_box(ilst.p, ilst.n, &q, &item) == 0) {
            const uint8_t *p;
            uint32_t n;
            if (!memcmp(item.type, "\xA9lyr", 4) && item_data(&item, &p, &n) == 0 && n) {
                copy_text(out, cap, p, n);
                found = 1;
            }
        }
    free(buf);
    return found ? 0 : -1;
}

static void free_track(Mp4Track *t)
{
    free(t->sizes);
    free(t->chunk_off);
    free(t->chunk_first);
    free(t->stts);
    free(t->sync);
    memset(t, 0, sizeof(*t));
}

void mp4_close(Mp4 *m)
{
    free_track(&m->video);
    free_track(&m->audio);
}

/* ------------------------------------------------------------------ H.264 */

typedef struct {
    const uint8_t *p;
    int n, bit; /* bit position, after emulation-prevention bytes are skipped */
    int zeros;
} Bits;

static int bits_u1(Bits *b)
{
    int byte = b->bit >> 3;
    if (byte >= b->n) return -1;
    int v = (b->p[byte] >> (7 - (b->bit & 7))) & 1;
    b->bit++;
    return v;
}

static int bits_ue(Bits *b)
{
    int zeros = 0;
    for (;;) {
        int v = bits_u1(b);
        if (v < 0 || zeros > 31) return -1;
        if (v) break;
        zeros++;
    }
    unsigned val = 0;
    for (int i = 0; i < zeros; i++) {
        int v = bits_u1(b);
        if (v < 0) return -1;
        val = val << 1 | v;
    }
    return (int)((1u << zeros) - 1 + val);
}

static int bits_se(Bits *b)
{
    int k = bits_ue(b);
    if (k < 0) return 0;
    return k & 1 ? (k + 1) / 2 : -(k / 2);
}

/* NAL payload without its header byte and emulation-prevention bytes (00 00 03). */
static int to_rbsp(const uint8_t *nal, int size, uint8_t *out, int cap)
{
    int n = 0, zeros = 0;
    for (int i = 1; i < size && n < cap; i++) {
        if (zeros >= 2 && nal[i] == 3) { zeros = 0; continue; }
        zeros = nal[i] == 0 ? zeros + 1 : 0;
        out[n++] = nal[i];
    }
    return n;
}

int h264_sps_refs(const uint8_t *sps, int size)
{
    uint8_t rbsp[256];
    int n = to_rbsp(sps, size, rbsp, sizeof(rbsp));
    if (n < 4) return -1;
    int profile = rbsp[0];
    Bits b = {rbsp, n, 24, 0}; /* after profile, constraint flags, level */
    if (bits_ue(&b) < 0) return -1; /* sps id */
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
        profile == 139 || profile == 134 || profile == 135) {
        int chroma = bits_ue(&b);
        if (chroma == 3) bits_u1(&b);
        bits_ue(&b); /* bit depths */
        bits_ue(&b);
        bits_u1(&b);
        if (bits_u1(&b) != 0) return -1; /* scaling matrices: not parsed */
    }
    if (bits_ue(&b) < 0) return -1; /* log2_max_frame_num_minus4 */
    int poc = bits_ue(&b);
    if (poc == 0) {
        if (bits_ue(&b) < 0) return -1;
    } else if (poc == 1) {
        bits_u1(&b);
        bits_se(&b);
        bits_se(&b);
        int cycle = bits_ue(&b);
        if (cycle < 0 || cycle > 255) return -1;
        for (int i = 0; i < cycle; i++) bits_se(&b);
    } else if (poc != 2) {
        return -1;
    }
    return bits_ue(&b);
}

int h264_pps_weights(const uint8_t *pps, int size, int *wp, int *wb)
{
    uint8_t rbsp[256];
    int n = to_rbsp(pps, size, rbsp, sizeof(rbsp));
    Bits b = {rbsp, n, 0, 0};
    if (bits_ue(&b) < 0 || bits_ue(&b) < 0) return -1; /* pps id, sps id */
    if (bits_u1(&b) < 0 || bits_u1(&b) < 0) return -1; /* entropy coding, bottom_field_pic_order */
    int groups = bits_ue(&b);
    if (groups != 0) return -1;                          /* slice groups: Baseline FMO, not seen in practice */
    if (bits_ue(&b) < 0 || bits_ue(&b) < 0) return -1;   /* num_ref_idx l0/l1 default */
    int p = bits_u1(&b), b0 = bits_u1(&b), b1 = bits_u1(&b);
    if (p < 0 || b0 < 0 || b1 < 0) return -1;
    *wp = p;
    *wb = b0 << 1 | b1;
    return 0;
}

int h264_sample_has_idr(const uint8_t *s, uint32_t size, int nal_length)
{
    uint32_t pos = 0;
    while (pos + (uint32_t)nal_length < size) {
        uint32_t len = 0;
        for (int k = 0; k < nal_length; k++) len = len << 8 | s[pos + k];
        pos += nal_length;
        if (len == 0 || pos + len > size) return 0;
        if ((s[pos] & 0x1F) == 5) return 1;
        pos += len;
    }
    return 0;
}

/* ------------------------------------------------------------------ access */

uint32_t mp4_size(const Mp4Track *t, uint32_t i)
{
    return t->fixed_size ? t->fixed_size : i < t->count ? t->sizes[i] : 0;
}

void mp4_chunk_of(const Mp4Track *t, uint32_t i, uint32_t *first, uint32_t *count)
{
    uint32_t lo = 0, hi = t->nchunks;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (t->chunk_first[mid] <= i) lo = mid;
        else hi = mid;
    }
    uint32_t end = lo + 1 < t->nchunks ? t->chunk_first[lo + 1] : t->count;
    if (end > t->count) end = t->count;
    *first = t->chunk_first[lo];
    *count = end > *first ? end - *first : 1;
}

uint64_t mp4_offset(const Mp4Track *t, uint32_t i)
{
    /* binary search: the chunk holding sample i */
    uint32_t lo = 0, hi = t->nchunks;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (t->chunk_first[mid] <= i) lo = mid;
        else hi = mid;
    }
    uint64_t off = t->chunk_off[lo];
    for (uint32_t s = t->chunk_first[lo]; s < i; s++)
        off += mp4_size(t, s);
    return off;
}

uint32_t mp4_time_ms(const Mp4Track *t, uint32_t i)
{
    uint64_t ticks = 0;
    for (uint32_t e = 0; e < t->nstts && i; e++) {
        uint32_t n = t->stts[e][0] < i ? t->stts[e][0] : i;
        ticks += (uint64_t)n * t->stts[e][1];
        i -= n;
    }
    return t->timescale ? (uint32_t)(ticks * 1000 / t->timescale) : 0;
}

uint32_t mp4_sample_at(const Mp4Track *t, uint32_t ms)
{
    uint64_t target = (uint64_t)ms * t->timescale / 1000, ticks = 0;
    uint32_t i = 0;
    for (uint32_t e = 0; e < t->nstts; e++) {
        uint64_t span = (uint64_t)t->stts[e][0] * t->stts[e][1];
        if (ticks + span > target) {
            uint32_t d = t->stts[e][1];
            i += d ? (uint32_t)((target - ticks) / d) : 0;
            return i < t->count ? i : t->count - 1;
        }
        ticks += span;
        i += t->stts[e][0];
    }
    return t->count ? t->count - 1 : 0;
}

int mp4_is_sync(const Mp4Track *t, uint32_t i)
{
    if (!t->sync) return 1;
    uint32_t lo = 0, hi = t->nsync;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (t->sync[mid] < i) lo = mid + 1;
        else hi = mid;
    }
    return lo < t->nsync && t->sync[lo] == i;
}

uint32_t mp4_sync_before(const Mp4Track *t, uint32_t i)
{
    if (!t->sync || !t->nsync) return i;
    uint32_t best = t->sync[0];
    for (uint32_t k = 0; k < t->nsync && t->sync[k] <= i; k++) /* small table: linear is fine */
        best = t->sync[k];
    return best;
}

uint32_t mp4_sync_after(const Mp4Track *t, uint32_t i)
{
    if (i >= t->count) return t->count;
    if (!t->sync || !t->nsync) return i;
    for (uint32_t k = 0; k < t->nsync; k++)
        if (t->sync[k] >= i)
            return t->sync[k];
    return t->count;
}

/* ------------------------------------------------- what the PSP won't play */

const char H264_PSP_FIX[] =
    "Re-encode it for the PSP: H.264 Main profile, level 3.0, 480x272, 3 reference frames, "
    "weighted prediction off (ffmpeg: -profile:v main -level 3.0 -refs 3 -x264-params weightp=0).";

int h264_psp_problem(const Mp4 *m, int weights_stripped, char *why, int n)
{
    int refs = h264_sps_refs(m->sps, m->sps_size), wp = -1, wb = -1;
    h264_pps_weights(m->pps, m->pps_size, &wp, &wb);
    if (wp == 1 && !weights_stripped)
        snprintf(why, n, "It uses weighted prediction, which the PSP can't decode.");
    else if (refs > 3)
        snprintf(why, n, "It uses %d reference frames; the PSP decodes 3 at most.", refs);
    else if (m->profile >= 100)
        snprintf(why, n, "It's H.264 High profile; the PSP plays Main and Baseline.");
    else if (m->level > 30)
        snprintf(why, n, "It's encoded at level %d.%d; the PSP plays up to 3.0.", m->level / 10, m->level % 10);
    else if (m->width > 720 || m->height > 480)
        snprintf(why, n, "It's %dx%d; the PSP plays up to 720x480.", m->width, m->height);
    else
        return 0;
    return 1;
}
