/*
 * Dynacolor MVC demuxer
 * Copyright (c) 2020-2026 Tom Needham
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "libavcodec/defs.h"
#include "libavcodec/get_bits.h"
#include "libavcodec/golomb.h"
#include "libavcodec/h264.h"
#include "libavcodec/sei.h"
#include "libavcodec/startcode.h"

#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "dynacolor.h"
#include "internal.h"

typedef struct DynacolorRecord {
    int64_t  pos;
    int64_t  data_pos;
    uint32_t size;
    uint32_t time;
    int      channel;
    int      pal;
    int      pic_type;
    int      format;
} DynacolorRecord;

typedef struct DynacolorStream {
    int64_t next_dts;
    /* first indexed record at or after the last seek target, and its dts */
    int64_t resume_pos;
    int64_t resume_dts;
    /* the timestamps are those of linear reading from the start */
    int     exact;
    int     codec_change_reported;

    /* H.264: max_num_ref_frames + 1 of each SPS, and the SPS id and
     * num_ref_idx_l0_default_active_minus1 + 1 of each PPS, 0 if unknown */
    uint8_t sps_ref_frames[H264_MAX_SPS_COUNT];
    uint8_t pps_sps[H264_MAX_PPS_COUNT];
    uint8_t pps_ref_idx[H264_MAX_PPS_COUNT];
} DynacolorStream;

/* Enough for the parameter set fields that are read, even after the
 * largest scaling matrices. */
#define DYNACOLOR_RBSP_SIZE 2048

typedef struct DynacolorDemuxContext {
    DynacolorRecord rec;
    uint32_t        start_time;
    int64_t         next_pos;
    int64_t         first_pos;
    int             seeked;
    unsigned        unsupported_reported;
} DynacolorDemuxContext;

static int dynacolor_probe(const AVProbeData *p)
{
    int64_t pos = 0;

    while (p->buf_size - pos >= DYNACOLOR_BASIC_IDX_SIZE) {
        const uint8_t *b = p->buf + pos;
        uint32_t size = AV_RL32(b + 4);
        int offset = DYNACOLOR_BASIC_IDX_SIZE;

        if (b[0] != '@' || ff_dynacolor_checksum(b, 15) != b[15])
            break;

        if (b[13] & 1) {
            if (p->buf_size - pos < offset + DYNACOLOR_EXTRA_IDX_SIZE ||
                ff_dynacolor_checksum(b + offset, 15) != b[offset + 15])
                break;
            offset += DYNACOLOR_EXTRA_IDX_SIZE + AV_RL16(b + offset + 2);
        }

        if (b[12] & 4) {
            pos += offset + (int64_t)size;
            continue;
        }

        if (size < DYNACOLOR_PES_HEADER_SIZE || p->buf_size - pos < offset + 4 ||
            AV_RB24(b + offset) != 1 || b[offset + 3] >> 4 < DYNACOLOR_FORMAT_AUDIO)
            break;

        return AVPROBE_SCORE_MAX;
    }

    return 0;
}

static int read_record_header(AVFormatContext *s, DynacolorRecord *rec)
{
    AVIOContext *pb = s->pb;
    uint8_t basic[DYNACOLOR_BASIC_IDX_SIZE];
    uint8_t extra[DYNACOLOR_EXTRA_IDX_SIZE];
    uint8_t pes[DYNACOLOR_PES_HEADER_SIZE];
    int64_t ret;

    rec->pos = avio_tell(pb);

    ret = avio_read(pb, basic, sizeof(basic));
    if (ret != sizeof(basic))
        return ret < 0 ? ret : AVERROR_EOF;

    if (basic[0] != '@') {
        av_log(s, AV_LOG_ERROR, "Invalid record header at 0x%"PRIx64"\n", rec->pos);
        return AVERROR_INVALIDDATA;
    }
    if (ff_dynacolor_checksum(basic, 15) != basic[15]) {
        av_log(s, AV_LOG_WARNING, "Record header checksum mismatch at 0x%"PRIx64"\n", rec->pos);
        if (s->error_recognition & AV_EF_EXPLODE)
            return AVERROR_INVALIDDATA;
    }

    rec->size     = AV_RL32(basic + 4);
    rec->time     = AV_RL32(basic + 8);
    rec->pal      = basic[12] & 1;
    rec->pic_type = basic[14] & 7;
    rec->channel  = basic[14] >> 3;
    /* Channel bits 5-6 are only valid if a DVR enables them; IP devices
     * use the same bits for left/right audio. */
    if ((basic[2] & 0x90) == 0x80)
        rec->channel |= (basic[3] & 3) << 5;

    if (basic[13] & 1) {
        ret = ffio_read_size(pb, extra, sizeof(extra));
        if (ret < 0)
            return avio_feof(pb) ? AVERROR_EOF : ret;
        if (ff_dynacolor_checksum(extra, 15) != extra[15]) {
            av_log(s, AV_LOG_WARNING, "Extra header checksum mismatch at 0x%"PRIx64"\n", rec->pos);
            if (s->error_recognition & AV_EF_EXPLODE)
                return AVERROR_INVALIDDATA;
        }
        ret = avio_skip(pb, AV_RL16(extra + 2));
        if (ret < 0)
            return ret;
    }

    /* Audio records can carry their payload directly, without a PES header. */
    if (basic[12] & 4) {
        rec->format = DYNACOLOR_FORMAT_AUDIO;
        rec->data_pos = avio_tell(pb);
        return 0;
    }

    if (rec->size < DYNACOLOR_PES_HEADER_SIZE) {
        av_log(s, AV_LOG_ERROR, "Invalid record size %"PRIu32"\n", rec->size);
        return AVERROR_INVALIDDATA;
    }
    ret = ffio_read_size(pb, pes, sizeof(pes));
    if (ret < 0)
        return avio_feof(pb) ? AVERROR_EOF : ret;
    if (AV_RB24(pes) != 1) {
        av_log(s, AV_LOG_ERROR, "Invalid PES header at 0x%"PRIx64"\n", rec->pos);
        return AVERROR_INVALIDDATA;
    }

    rec->format = pes[3] >> 4;
    rec->size -= DYNACOLOR_PES_HEADER_SIZE;
    if (rec->format == DYNACOLOR_FORMAT_JPEG)
        rec->pic_type = DYNACOLOR_PICTURE_I;

    rec->data_pos = avio_tell(pb);

    return 0;
}

typedef struct RBSPReader {
    const uint8_t *p, *end;
    int zeros;
} RBSPReader;

/* Read a byte of a NAL unit payload, skipping emulation prevention bytes.
 * Return -1 at the end of the NAL unit. */
static int rbsp_byte(RBSPReader *r)
{
    while (r->p < r->end) {
        if (r->zeros >= 2) {
            if (*r->p == 3) {
                r->zeros = 0;
                r->p++;
                continue;
            }
            if (*r->p <= 2)
                return -1;
        }
        r->zeros = *r->p ? 0 : r->zeros + 1;
        return *r->p++;
    }
    return -1;
}

/* Read the payloadType or payloadSize value of an SEI message. */
static int rbsp_sei_value(RBSPReader *r)
{
    int value = 0, b;

    do {
        b = rbsp_byte(r);
        if (b < 0 || value > INT_MAX - b)
            return -1;
        value += b;
    } while (b == 0xFF);
    return value;
}

/* A maximal recovery_frame_cnt and its four following bits fit in 5 bytes. */
static int rbsp_recovery_point(RBSPReader *r, int size)
{
    uint8_t data[5 + AV_INPUT_BUFFER_PADDING_SIZE] = { 0 };
    GetBitContext gb;
    unsigned recovery_frame_cnt;

    if (!size)
        return 0;
    for (int i = 0; i < size; i++) {
        int b = rbsp_byte(r);
        if (b < 0)
            return 0;
        if (i < 5)
            data[i] = b;
    }
    init_get_bits8(&gb, data, FFMIN(size, 5));
    recovery_frame_cnt = get_ue_golomb_long(&gb);
    /* The maximum MaxFrameNum in H.264 is 2^16. */
    return recovery_frame_cnt < (1U << 16) && get_bits_left(&gb) >= 4;
}

/* Unescape the payload of the NAL unit starting at p, up to size bytes. */
static int rbsp_read(const uint8_t *p, const uint8_t *end, uint8_t *buf, int size)
{
    RBSPReader r = { p, end };
    int len = 0, b;

    while (len < size && (b = rbsp_byte(&r)) >= 0)
        buf[len++] = b;
    memset(buf + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    return len;
}

static void skip_scaling_list(GetBitContext *gb, int size)
{
    int last = 8, next = 8;

    for (int i = 0; i < size && next; i++) {
        next = (last + get_se_golomb(gb)) & 0xFF;
        if (next)
            last = next;
    }
}

/* Keep max_num_ref_frames of an SPS. */
static void h264_parse_sps(DynacolorStream *dst, const uint8_t *buf, int size)
{
    GetBitContext gb;
    unsigned profile, sps_id, poc_type, ref_frames;

    init_get_bits8(&gb, buf, size);
    profile = get_bits(&gb, 8);
    skip_bits(&gb, 16); /* constraint flags, level_idc */
    sps_id  = get_ue_golomb_31(&gb);
    if (sps_id >= H264_MAX_SPS_COUNT)
        return;
    dst->sps_ref_frames[sps_id] = 0;

    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
        profile ==  44 || profile ==  83 || profile ==  86 || profile == 118 ||
        profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        unsigned chroma_format_idc = get_ue_golomb_31(&gb);
        if (chroma_format_idc == 3)
            skip_bits1(&gb);          /* separate_colour_plane_flag */
        get_ue_golomb_31(&gb);        /* bit_depth_luma_minus8 */
        get_ue_golomb_31(&gb);        /* bit_depth_chroma_minus8 */
        skip_bits1(&gb);              /* qpprime_y_zero_transform_bypass_flag */
        if (get_bits1(&gb)) {         /* seq_scaling_matrix_present_flag */
            for (int i = 0; i < (chroma_format_idc == 3 ? 12 : 8); i++)
                if (get_bits1(&gb))
                    skip_scaling_list(&gb, i < 6 ? 16 : 64);
        }
    }
    get_ue_golomb_long(&gb);          /* log2_max_frame_num_minus4 */
    poc_type = get_ue_golomb_long(&gb);
    if (poc_type == 0) {
        get_ue_golomb_long(&gb);      /* log2_max_pic_order_cnt_lsb_minus4 */
    } else if (poc_type == 1) {
        unsigned cycle;
        skip_bits1(&gb);              /* delta_pic_order_always_zero_flag */
        get_se_golomb_long(&gb);      /* offset_for_non_ref_pic */
        get_se_golomb_long(&gb);      /* offset_for_top_to_bottom_field */
        cycle = get_ue_golomb_long(&gb);
        if (cycle > 255)
            return;
        for (unsigned i = 0; i < cycle; i++)
            get_se_golomb_long(&gb);  /* offset_for_ref_frame */
    } else if (poc_type > 2) {
        return;
    }
    ref_frames = get_ue_golomb_long(&gb);
    if (get_bits_left(&gb) < 0 || ref_frames > 16)
        return;
    dst->sps_ref_frames[sps_id] = ref_frames + 1;
}

/* Keep the SPS id and num_ref_idx_l0_default_active_minus1 of a PPS. */
static void h264_parse_pps(DynacolorStream *dst, const uint8_t *buf, int size)
{
    GetBitContext gb;
    unsigned pps_id, sps_id, slice_groups, ref_idx;

    init_get_bits8(&gb, buf, size);
    pps_id = get_ue_golomb_long(&gb);
    if (pps_id >= H264_MAX_PPS_COUNT)
        return;
    dst->pps_sps[pps_id] = dst->pps_ref_idx[pps_id] = 0;
    sps_id = get_ue_golomb_long(&gb);
    if (sps_id >= H264_MAX_SPS_COUNT)
        return;
    skip_bits(&gb, 2);  /* entropy_coding_mode_flag, bottom_field_pic_order_in_frame_present_flag */
    slice_groups = get_ue_golomb_long(&gb) + 1;
    if (slice_groups > 8)
        return;
    if (slice_groups > 1) {
        switch (get_ue_golomb_long(&gb)) { /* slice_group_map_type */
        case 0:
            for (unsigned i = 0; i < slice_groups; i++)
                get_ue_golomb_long(&gb);   /* run_length_minus1 */
            break;
        case 2:
            for (unsigned i = 0; i < slice_groups - 1; i++) {
                get_ue_golomb_long(&gb);   /* top_left */
                get_ue_golomb_long(&gb);   /* bottom_right */
            }
            break;
        case 3:
        case 4:
        case 5:
            skip_bits1(&gb);               /* slice_group_change_direction_flag */
            get_ue_golomb_long(&gb);       /* slice_group_change_rate_minus1 */
            break;
        case 6: {
            unsigned units = get_ue_golomb_long(&gb) + 1;
            if (get_bits_left(&gb) < 0 || units > get_bits_left(&gb))
                return;
            skip_bits_long(&gb, units * av_ceil_log2(slice_groups));
            break;
        }
        case 1:
            break;
        default:
            return;
        }
    }
    ref_idx = get_ue_golomb_long(&gb);
    if (get_bits_left(&gb) < 0 || ref_idx > 31)
        return;
    dst->pps_sps[pps_id]     = sps_id;
    dst->pps_ref_idx[pps_id] = ref_idx + 1;
}

/* Check whether an I-picture is a keyframe by the heuristic of the H.264
 * parser: with a single reference frame, later pictures cannot refer to
 * pictures before it. */
static int h264_is_intra_keyframe(const DynacolorStream *dst, const uint8_t *buf, int size)
{
    GetBitContext gb;
    unsigned slice_type, pps_id;

    init_get_bits8(&gb, buf, size);
    get_ue_golomb_long(&gb);            /* first_mb_in_slice */
    slice_type = get_ue_golomb_31(&gb);
    pps_id     = get_ue_golomb_long(&gb);
    if (get_bits_left(&gb) < 0 || slice_type > 9 || slice_type % 5 != 2 ||
        pps_id >= H264_MAX_PPS_COUNT || !dst->pps_ref_idx[pps_id])
        return 0;
    return dst->pps_ref_idx[pps_id] <= 1 &&
           dst->sps_ref_frames[dst->pps_sps[pps_id]] &&
           dst->sps_ref_frames[dst->pps_sps[pps_id]] <= 2;
}

/*
 * Check whether decoding can start at an H.264 access unit, as the H.264
 * parser does: its first slice is an IDR slice, or is preceded by a recovery
 * point SEI message, or is an I-slice using a single reference frame. The
 * parameter sets before it are kept for the latter.
 */
static int h264_is_random_access_point(DynacolorStream *dst, const uint8_t *buf, int size)
{
    uint8_t rbsp[DYNACOLOR_RBSP_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    const uint8_t *p = buf, *end;
    uint32_t state = -1;
    int recovery = 0;

    if (!size)
        return 0;
    end = buf + size;
    while ((p = avpriv_find_start_code(p, end, &state)) < end) {
        RBSPReader r = { p, end };
        int type = state & 0x1F;
        uint32_t next_state = -1;
        const uint8_t *next;

        if (type == H264_NAL_IDR_SLICE)
            return 1;
        if (type == H264_NAL_SLICE || type == H264_NAL_DPA)
            return recovery ||
                   h264_is_intra_keyframe(dst, rbsp, rbsp_read(p, end, rbsp, 16));
        if (type == H264_NAL_SPS)
            h264_parse_sps(dst, rbsp, rbsp_read(p, end, rbsp, DYNACOLOR_RBSP_SIZE));
        else if (type == H264_NAL_PPS)
            h264_parse_pps(dst, rbsp, rbsp_read(p, end, rbsp, DYNACOLOR_RBSP_SIZE));
        if (recovery || type != H264_NAL_SEI)
            continue;

        next = avpriv_find_start_code(p, end, &next_state);
        if ((next_state & 0xFFFFFF00) == 0x100)
            r.end = next - 4;
        while (r.end > p && !r.end[-1])
            r.end--;
        /* The byte-aligned SEI messages are followed by rbsp_trailing_bits. */
        if (r.end == p || r.end[-1] != 0x80)
            continue;
        r.end--;

        for (;;) {
            int payload_type = rbsp_sei_value(&r);
            int payload_size;

            if (payload_type < 0)
                break;
            payload_size = rbsp_sei_value(&r);
            if (payload_size < 0 || payload_size > r.end - r.p)
                break;
            if (payload_type == SEI_TYPE_RECOVERY_POINT) {
                if (rbsp_recovery_point(&r, payload_size)) {
                    recovery = 1;
                    break;
                }
            } else {
                while (payload_size-- > 0 && rbsp_byte(&r) >= 0)
                    ;
            }
        }
    }
    return 0;
}

static int get_stream(AVFormatContext *s, const DynacolorRecord *rec, AVStream **pst)
{
    DynacolorDemuxContext *dyn = s->priv_data;
    enum AVCodecID codec_id;
    AVStream *st;

    *pst = NULL;

    switch (rec->format) {
    case DYNACOLOR_FORMAT_JPEG:  codec_id = AV_CODEC_ID_MJPEG; break;
    case DYNACOLOR_FORMAT_MPEG4: codec_id = AV_CODEC_ID_MPEG4; break;
    case DYNACOLOR_FORMAT_H264:  codec_id = AV_CODEC_ID_H264;  break;
    default:
        if (!(dyn->unsupported_reported & (1U << rec->format))) {
            if (rec->format == DYNACOLOR_FORMAT_AUDIO)
                avpriv_request_sample(s, "Audio");
            else
                avpriv_request_sample(s, "Format 0x%X", rec->format);
            dyn->unsupported_reported |= 1U << rec->format;
        }
        return 0;
    }

    for (unsigned i = 0; i < s->nb_streams; i++) {
        if (s->streams[i]->id == rec->channel) {
            st = s->streams[i];
            if (st->codecpar->codec_id != codec_id) {
                DynacolorStream *dst = st->priv_data;
                if (!dst->codec_change_reported) {
                    av_log(s, AV_LOG_WARNING, "Codec change on channel %d is not supported\n",
                           rec->channel);
                    dst->codec_change_reported = 1;
                }
                return 0;
            }
            *pst = st;
            return 0;
        }
    }

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);
    st->priv_data = av_mallocz(sizeof(DynacolorStream));
    if (!st->priv_data) {
        ff_remove_stream(s, st);
        return AVERROR(ENOMEM);
    }
    DynacolorStream *dyna_stream = ((DynacolorStream *)st->priv_data);
    dyna_stream->next_dts   = AV_NOPTS_VALUE;
    dyna_stream->resume_pos = -1;
    /* A channel found after a seek may have started earlier in the file. */
    dyna_stream->exact      = !dyn->seeked;

    st->id                   = rec->channel;
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = codec_id;
    /* Late channels also need reordering information for generic PTS filling. */
    if (codec_id == AV_CODEC_ID_MPEG4)
        ffstream(st)->need_parsing = AVSTREAM_PARSE_HEADERS;
    st->r_frame_rate         = (AVRational){ rec->pal ? 25 : 30, 1 };
    avpriv_set_pts_info(st, 64, 1, rec->pal ? 25 : 30);

    *pst = st;
    return 0;
}

static int dynacolor_read_header(AVFormatContext *s)
{
    DynacolorDemuxContext *dyn = s->priv_data;
    DynacolorRecord *rec = &dyn->rec;
    AVStream *st;
    int ret;

    ret = ffio_ensure_seekback(s->pb, DYNACOLOR_BASIC_IDX_SIZE + DYNACOLOR_EXTRA_IDX_SIZE +
                                      UINT16_MAX + DYNACOLOR_PES_HEADER_SIZE);
    if (ret < 0)
        return ret;
    ret = read_record_header(s, rec);
    if (ret < 0)
        return ret;
    dyn->next_pos   = rec->pos;
    dyn->first_pos  = rec->pos;
    dyn->start_time = rec->time;

    ret = ff_dict_set_timestamp(&s->metadata, "creation_time",
                                dyn->start_time * INT64_C(1000000));
    if (ret < 0)
        return ret;

    /* More channels may show up later in the file. */
    s->ctx_flags |= AVFMTCTX_NOHEADER;

    ret = get_stream(s, rec, &st);
    if (ret < 0)
        return ret;

    /* Leave the first record to read_packet(), the data starts there. */
    ret = avio_seek(s->pb, rec->pos, SEEK_SET);
    return ret < 0 ? ret : 0;
}

/*
 * After a seek to pos, find the first record of each channel at or after
 * pos that the index holds, so that its timestamp can be restored once it
 * is reached. A seek to the first record restarts linear reading.
 */
static void seek_reset(AVFormatContext *s, int64_t pos)
{
    DynacolorDemuxContext *dyn = s->priv_data;
    int restart = pos == dyn->first_pos;

    dyn->seeked = !restart;

    for (unsigned i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        DynacolorStream *dst = st->priv_data;
        int nb_entries = avformat_index_get_entries_count(st);
        int lo = 0, hi = nb_entries;

        dst->next_dts   = AV_NOPTS_VALUE;
        dst->resume_pos = -1;
        dst->exact      = restart;
        if (restart)
            continue;
        /* Only exact timestamps are indexed, so the entries of a channel are
         * in file order too. */
        while (lo < hi) {
            int mid = (lo + hi) >> 1;
            if (avformat_index_get_entry(st, mid)->pos < pos)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < nb_entries) {
            const AVIndexEntry *e = avformat_index_get_entry(st, lo);
            dst->resume_pos = e->pos;
            dst->resume_dts = e->timestamp;
        }
    }
}

static int dynacolor_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    DynacolorDemuxContext *dyn = s->priv_data;
    DynacolorRecord *rec = &dyn->rec;
    AVIOContext *pb = s->pb;
    DynacolorStream *dst;
    DynacolorRecord cur;
    AVStream *st;
    int64_t dts, skip;
    int ret;

    for (;;) {
        ret = read_record_header(s, rec);
        if (ret < 0)
            return ret;

        if (rec->pos != dyn->next_pos)
            seek_reset(s, rec->pos);
        dyn->next_pos = rec->data_pos + rec->size;

        ret = get_stream(s, rec, &st);
        if (ret < 0)
            return ret;
        if (st)
            break;

        skip = avio_skip(pb, rec->size);
        if (skip < 0)
            return skip;
    }

    cur = *rec;
    ret = av_get_packet(pb, pkt, cur.size);
    if (ret < 0)
        return ret;

    /*
     * Records only carry a timestamp in seconds. Frames are placed on a
     * grid of the nominal PAL/NTSC frame rate, in decoding order, starting
     * at the beginning of the second they belong to, so that the timestamps
     * of demuxed packets written by the muxer are reproduced. Other timing
     * within a second is lost. Beyond 25 or 30 records per second of a
     * channel, the following timestamps are pushed back: frame rates above
     * 30 fps drift.
     *
     * After a seek, the timestamps of each channel are restored from the
     * first of its records held by the index. Records before it are only
     * known with a granularity of one second; the estimates never exceed
     * the timestamps of linear reading.
     *
     * There is no presentation time. The parser derives it for MPEG-4, but
     * H.264 packets with B-pictures only have a decoding timestamp.
     */
    dst = st->priv_data;
    if (dst->resume_pos >= 0 && cur.pos >= dst->resume_pos) {
        if (cur.pos == dst->resume_pos) {
            dst->next_dts = FFMAX(dst->next_dts, dst->resume_dts);
            dst->exact    = 1;
        }
        dst->resume_pos = -1;
    }
    dts = ((int64_t)cur.time - dyn->start_time) * st->time_base.den;
    if (dst->next_dts != AV_NOPTS_VALUE)
        dts = FFMAX(dts, dst->next_dts);
    dst->next_dts = dts + 1;

    pkt->stream_index = st->index;
    pkt->pos          = cur.pos;
    pkt->dts          = dts;
    /* H.264 decoding cannot start at any I-picture: keyframes are taken
     * from the bitstream, as the parser does. */
    if (st->codecpar->codec_id == AV_CODEC_ID_H264 ?
        h264_is_random_access_point(dst, pkt->data, pkt->size) :
        cur.pic_type == DYNACOLOR_PICTURE_I) {
        pkt->flags |= AV_PKT_FLAG_KEY;
        /* Only exact timestamps are indexed, estimates must not be reused. */
        if (dst->exact) {
            ff_reduce_index(s, st->index);
            av_add_index_entry(st, cur.pos, dts, 0, 0, AVINDEX_KEYFRAME);
        }
    }

    return 0;
}

const FFInputFormat ff_dynacolor_demuxer = {
    .p.name         = "dynacolor",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Dynacolor MVC"),
    .p.extensions   = "dyna,dyn",
    .p.flags        = AVFMT_NO_BYTE_SEEK,
    .priv_data_size = sizeof(DynacolorDemuxContext),
    .read_probe     = dynacolor_probe,
    .read_header    = dynacolor_read_header,
    .read_packet    = dynacolor_read_packet,
};
