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
} DynacolorStream;

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
    const uint8_t *b = p->buf;
    int offset = DYNACOLOR_BASIC_IDX_SIZE;

    if (p->buf_size < DYNACOLOR_BASIC_IDX_SIZE + DYNACOLOR_EXTRA_IDX_SIZE + 4)
        return 0;
    if (b[0] != '@' || ff_dynacolor_checksum(b, 15) != b[15] ||
        AV_RL32(b + 4) < DYNACOLOR_PES_HEADER_SIZE)
        return 0;

    if (b[13] & 1) {
        if (ff_dynacolor_checksum(b + offset, 15) != b[offset + 15])
            return 0;
        offset += DYNACOLOR_EXTRA_IDX_SIZE + AV_RL16(b + offset + 2);
    }

    if (offset + 4 > p->buf_size ||
        AV_RB24(b + offset) != 1 || b[offset + 3] >> 4 < DYNACOLOR_FORMAT_AUDIO)
        return 0;

    return AVPROBE_SCORE_MAX;
}

static int read_record_header(AVFormatContext *s, DynacolorRecord *rec)
{
    AVIOContext *pb = s->pb;
    uint8_t basic[DYNACOLOR_BASIC_IDX_SIZE];
    uint8_t extra[DYNACOLOR_EXTRA_IDX_SIZE];
    uint8_t pes[DYNACOLOR_PES_HEADER_SIZE];
    int ret;

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
    rec->channel  = (basic[3] & 3) << 5 | basic[14] >> 3;

    if (rec->size < DYNACOLOR_PES_HEADER_SIZE) {
        av_log(s, AV_LOG_ERROR, "Invalid record size %"PRIu32"\n", rec->size);
        return AVERROR_INVALIDDATA;
    }

    if (basic[13] & 1) {
        ret = ffio_read_size(pb, extra, sizeof(extra));
        if (ret < 0)
            return avio_feof(pb) ? AVERROR_EOF : ret;
        if (ff_dynacolor_checksum(extra, 15) != extra[15]) {
            av_log(s, AV_LOG_WARNING, "Extra header checksum mismatch at 0x%"PRIx64"\n", rec->pos);
            if (s->error_recognition & AV_EF_EXPLODE)
                return AVERROR_INVALIDDATA;
        }
        avio_skip(pb, AV_RL16(extra + 2));
    }

    ret = ffio_read_size(pb, pes, sizeof(pes));
    if (ret < 0)
        return avio_feof(pb) ? AVERROR_EOF : ret;
    if (AV_RB24(pes) != 1) {
        av_log(s, AV_LOG_ERROR, "Invalid PES header at 0x%"PRIx64"\n", rec->pos);
        return AVERROR_INVALIDDATA;
    }

    rec->format = pes[3] >> 4;
    if (basic[12] & 4)
        rec->format = DYNACOLOR_FORMAT_AUDIO;
    if (rec->format == DYNACOLOR_FORMAT_JPEG)
        rec->pic_type = DYNACOLOR_PICTURE_I;

    rec->data_pos = avio_tell(pb);

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
    if (!st->priv_data)
        return AVERROR(ENOMEM);
    ((DynacolorStream *)st->priv_data)->next_dts   = AV_NOPTS_VALUE;
    ((DynacolorStream *)st->priv_data)->resume_pos = -1;
    /* A channel found after a seek may have started earlier in the file. */
    ((DynacolorStream *)st->priv_data)->exact      = !dyn->seeked;

    st->id                  = rec->channel;
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

/* Find the timestamp the generic index holds for the record at pos. */
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

        dst->next_dts   = AV_NOPTS_VALUE;
        dst->resume_pos = -1;
        dst->exact      = restart;
        if (restart)
            continue;
        for (int j = 0; j < nb_entries; j++) {
            const AVIndexEntry *e = avformat_index_get_entry(st, j);
            if (e->pos >= pos && (dst->resume_pos < 0 || e->pos < dst->resume_pos)) {
                dst->resume_pos = e->pos;
                dst->resume_dts = e->timestamp;
            }
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
    int64_t dts;
    int ret;

    for (;;) {
        ret = read_record_header(s, rec);
        if (ret < 0)
            return ret;

        if (rec->pos != dyn->next_pos)
            seek_reset(s, rec->pos);
        dyn->next_pos = rec->data_pos + rec->size - DYNACOLOR_PES_HEADER_SIZE;

        ret = get_stream(s, rec, &st);
        if (ret < 0)
            return ret;
        if (st)
            break;

        avio_skip(pb, rec->size - DYNACOLOR_PES_HEADER_SIZE);
    }

    cur = *rec;
    ret = av_get_packet(pb, pkt, cur.size - DYNACOLOR_PES_HEADER_SIZE);
    if (ret < 0)
        return ret;

    /*
     * Records only carry a timestamp in seconds. Frames are placed on a
     * grid of the nominal PAL/NTSC frame rate, in decoding order, starting
     * at the beginning of the second they belong to, so that the timestamps
     * written by the muxer are reproduced.
     *
     * After a seek, the timestamps of each channel are restored from the
     * first of its records held by the index. Records before it are only
     * known with a granularity of one second; the estimates never exceed
     * the timestamps of linear reading.
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
    if (cur.pic_type == DYNACOLOR_PICTURE_I) {
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
    .priv_data_size = sizeof(DynacolorDemuxContext),
    .read_probe     = dynacolor_probe,
    .read_header    = dynacolor_read_header,
    .read_packet    = dynacolor_read_packet,
};
