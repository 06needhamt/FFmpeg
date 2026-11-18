/*
 * Dynacolor MVC muxer
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
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"

#include "libavcodec/get_bits.h"
#include "libavcodec/golomb.h"
#include "libavcodec/h264.h"
#include "libavcodec/startcode.h"

#include "avformat.h"
#include "dynacolor.h"
#include "internal.h"
#include "mux.h"

typedef struct DynacolorMuxStream {
    int     format;
    int     pal;
    int     width_base;
    int64_t last_time;
} DynacolorMuxStream;

typedef struct DynacolorMuxContext {
    int64_t  start_time;
    uint32_t serial;
} DynacolorMuxContext;

/* Unused PES header bytes 6-23, as found in files written by Dynacolor devices. */
static const uint8_t pes_filler[18] = {
    0x87, 0x01, 0x17, 0x31, 0x00, 0x01, 0x00, 0x01, 0x11,
    0x00, 0x01, 0x00, 0x01, 0x0F, 0x8B, 0x00, 0x00, 0x00,
};

static int dynacolor_init(AVFormatContext *s)
{
    DynacolorMuxContext *dyn = s->priv_data;
    int ret;

    if (s->nb_streams > DYNACOLOR_MAX_CHANNELS) {
        av_log(s, AV_LOG_ERROR, "At most %d streams are supported\n",
               DYNACOLOR_MAX_CHANNELS);
        return AVERROR(EINVAL);
    }

    ret = ff_parse_creation_time_metadata(s, &dyn->start_time, 1);
    if (ret < 0)
        return ret;
    if (dyn->start_time < 0 || dyn->start_time > UINT32_MAX) {
        av_log(s, AV_LOG_ERROR, "creation_time out of range\n");
        return AVERROR(EINVAL);
    }

    for (unsigned i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        const AVCodecParameters *par = st->codecpar;
        DynacolorMuxStream *dst;
        AVRational rate;

        if (par->codec_type != AVMEDIA_TYPE_VIDEO) {
            av_log(s, AV_LOG_ERROR, "Only video streams are supported\n");
            return AVERROR_PATCHWELCOME;
        }

        dst = st->priv_data = av_mallocz(sizeof(*dst));
        if (!dst)
            return AVERROR(ENOMEM);
        dst->last_time = dyn->start_time;

        switch (par->codec_id) {
        case AV_CODEC_ID_MJPEG: dst->format = DYNACOLOR_FORMAT_JPEG;  break;
        case AV_CODEC_ID_MPEG4: dst->format = DYNACOLOR_FORMAT_MPEG4; break;
        case AV_CODEC_ID_H264:  dst->format = DYNACOLOR_FORMAT_H264;  break;
        default:
            av_log(s, AV_LOG_ERROR, "Codec %s is not supported\n",
                   avcodec_get_name(par->codec_id));
            return AVERROR(EINVAL);
        }

        /* Frame rates above 25 fps can only be represented by NTSC. */
        rate = st->r_frame_rate;
        if (rate.num <= 0 || rate.den <= 0)
            rate = st->avg_frame_rate;
        dst->pal = rate.num <= 0 || rate.den <= 0 || av_cmp_q(rate, (AVRational){ 25, 1 }) <= 0;

        switch (par->width) {
        case 704: dst->width_base = 1; break;
        case 640: dst->width_base = 2; break;
        default:  dst->width_base = 0; break;
        }
    }

    return 0;
}

/**
 * Find the type of the first picture of an MPEG-4 or H.264 packet.
 *
 * @param config set to 1 if the packet carries a VOL header or an SPS
 * @return DYNACOLOR_PICTURE_I/P/B, or 0 if no picture was found
 */
static int parse_packet(enum AVCodecID codec_id, const uint8_t *buf, int size,
                        int *config)
{
    const uint8_t *p = buf, *end = buf + size;
    uint32_t state = -1;

    *config = 0;
    while ((p = avpriv_find_start_code(p, end, &state)) < end) {
        if (codec_id == AV_CODEC_ID_MPEG4) {
            if (state >= 0x120 && state <= 0x12F) {
                *config = 1;
            } else if (state == 0x1B6) {
                switch (*p >> 6) {
                case 0:  return DYNACOLOR_PICTURE_I;
                case 2:  return DYNACOLOR_PICTURE_B;
                default: return DYNACOLOR_PICTURE_P;
                }
            }
        } else if (codec_id == AV_CODEC_ID_H264) {
            int nal_type = state & 0x1F;
            if (nal_type == 1 || nal_type == 5) {
                GetBitContext gb;
                if (init_get_bits8(&gb, p, end - p) < 0)
                    return 0;
                get_ue_golomb_long(&gb); /* first_mb_in_slice */
                switch (get_ue_golomb_long(&gb) % 5) {
                case 1:  return DYNACOLOR_PICTURE_B;
                case 2:
                case 4:  return DYNACOLOR_PICTURE_I;
                default: return DYNACOLOR_PICTURE_P;
                }
            }
        }
    }
    return 0;
}

/**
 * Find where the SPS and PPS must be inserted in an H.264 IDR access unit
 * that does not carry both of them in band: after its AUD if there is one,
 * as the AUD must come first, otherwise at its start.
 *
 * @return the insertion point, or NULL if nothing must be inserted
 */
static const uint8_t *h264_config_insert_point(const uint8_t *buf, int size)
{
    const uint8_t *p = buf, *end = buf + size, *insert = buf;
    uint32_t state = -1;
    int sps = 0, pps = 0, first = 1, after_aud = 0;

    while ((p = avpriv_find_start_code(p, end, &state)) < end) {
        int nal_type = state & 0x1F;
        const uint8_t *nal_start = p - 4;

        if (nal_start > buf && !nal_start[-1])
            nal_start--;
        if (after_aud) {
            insert    = nal_start;
            after_aud = 0;
        }
        switch (nal_type) {
        case H264_NAL_AUD: after_aud = first; break;
        case H264_NAL_SPS: sps = 1;           break;
        case H264_NAL_PPS: pps = 1;           break;
        case H264_NAL_IDR_SLICE:
            return sps && pps ? NULL : insert;
        case H264_NAL_SLICE:
            return NULL;
        }
        first = 0;
    }
    return NULL;
}

static int dynacolor_check_bitstream(AVFormatContext *s, AVStream *st,
                                     const AVPacket *pkt)
{
    const AVCodecParameters *par = st->codecpar;
    int config;

    if (par->codec_id == AV_CODEC_ID_H264 &&
      ((par->extradata_size > 0 && par->extradata[0] == 1) ||
       (pkt->size >= 5 && AV_RB32(pkt->data) != 0x00000001 &&
        AV_RB24(pkt->data) != 0x000001)))
        return ff_stream_add_bitstream_filter(st, "h264_mp4toannexb", NULL);

    /* H.264 parameter sets are inserted by dynacolor_write_packet(). */
    if (par->codec_id != AV_CODEC_ID_MPEG4 || !par->extradata_size)
        return 1;

    /* The decoder configuration must be repeated in band on keyframes, as
     * records have no global header. Add it only if it is not already there. */
    if (!(pkt->flags & AV_PKT_FLAG_KEY))
        return 0;
    parse_packet(par->codec_id, pkt->data, pkt->size, &config);
    if (config)
        return 1;
    return ff_stream_add_bitstream_filter(st, "dump_extra", NULL);
}

static int dynacolor_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    DynacolorMuxContext *dyn = s->priv_data;
    AVStream *st = s->streams[pkt->stream_index];
    DynacolorMuxStream *dst = st->priv_data;
    AVIOContext *pb = s->pb;
    uint8_t buf[DYNACOLOR_BASIC_IDX_SIZE + DYNACOLOR_EXTRA_IDX_SIZE + DYNACOLOR_PES_HEADER_SIZE];
    uint8_t *basic = buf;
    uint8_t *extra = basic + DYNACOLOR_BASIC_IDX_SIZE;
    uint8_t *pes   = extra + DYNACOLOR_EXTRA_IDX_SIZE;
    int channel    = pkt->stream_index;
    int64_t time;
    const AVCodecParameters *par = st->codecpar;
    const uint8_t *insert = NULL;
    int insert_size = 0;
    uint32_t size;
    int pic_type, config;

    /* Records have no global header: IDR pictures need in-band SPS and PPS. */
    if (par->codec_id == AV_CODEC_ID_H264 && pkt->flags & AV_PKT_FLAG_KEY &&
        par->extradata_size >= 4 && (AV_RB24(par->extradata) == 1 ||
                                     AV_RB32(par->extradata) == 1)) {
        insert = h264_config_insert_point(pkt->data, pkt->size);
        if (insert)
            insert_size = par->extradata_size;
    }

    if (pkt->size > DYNACOLOR_MAX_RECORD_SIZE - DYNACOLOR_PES_HEADER_SIZE - insert_size) {
        av_log(s, AV_LOG_ERROR, "Packet of size %d is too large\n", pkt->size);
        return AVERROR(EINVAL);
    }
    size = pkt->size + insert_size + DYNACOLOR_PES_HEADER_SIZE;

    time = dst->last_time;
    if (pkt->dts != AV_NOPTS_VALUE) {
        int64_t seconds = av_rescale_rnd(pkt->dts, st->time_base.num,
                                         st->time_base.den, AV_ROUND_DOWN);
        if (seconds < -dyn->start_time ||
            seconds > UINT32_MAX - dyn->start_time) {
            av_log(s, AV_LOG_ERROR, "Timestamp out of range\n");
            return AVERROR(EINVAL);
        }
        time = dyn->start_time + seconds;
    }
    dst->last_time = time;

    if (dst->format == DYNACOLOR_FORMAT_JPEG)
        pic_type = DYNACOLOR_PICTURE_I;
    else
        pic_type = parse_packet(st->codecpar->codec_id, pkt->data, pkt->size, &config);
    if (!pic_type)
        pic_type = pkt->flags & AV_PKT_FLAG_KEY ? DYNACOLOR_PICTURE_I : DYNACOLOR_PICTURE_P;

    memset(buf, 0, sizeof(buf));

    basic[0]  = '@';
    basic[1]  = '2';
    basic[2]  = dst->width_base << 5;
    basic[3]  = channel >> 5;
    AV_WL32(basic + 4, size);
    AV_WL32(basic + 8, time);
    basic[12] = dst->pal;
    basic[13] = 1; /* extra index present */
    basic[14] = (channel & 0x1F) << 3 | pic_type;
    basic[15] = ff_dynacolor_checksum(basic, 15);

    AV_WL32(extra + 4, dyn->serial++);
    extra[15] = ff_dynacolor_checksum(extra, 15);

    AV_WB24(pes, 1);
    pes[3]  = dst->format << 4 | (channel & 0xF);
    AV_WB16(pes + 4, size - 6 <= UINT16_MAX ? size - 6 : 0);
    memcpy(pes + 6, pes_filler, sizeof(pes_filler));
    pes[24] = 0x80 | (size >> 15 & 0x7F);
    pes[25] = (size >> 11 & 0xF) << 4 | 0x08 | (size >> 8 & 0x7);
    pes[26] = size & 0xFF;
    pes[27] = pic_type;
    AV_WN32(pes + 28, 0xFFFFFFFF);

    avio_write(pb, buf, sizeof(buf));
    if (insert) {
        avio_write(pb, pkt->data, insert - pkt->data);
        avio_write(pb, par->extradata, par->extradata_size);
        avio_write(pb, insert, pkt->data + pkt->size - insert);
    } else {
        avio_write(pb, pkt->data, pkt->size);
    }

    return 0;
}

const FFOutputFormat ff_dynacolor_muxer = {
    .p.name           = "dynacolor",
    .p.long_name      = NULL_IF_CONFIG_SMALL("Dynacolor MVC"),
    .p.extensions     = "dyna,dyn",
    .p.audio_codec    = AV_CODEC_ID_NONE,
    .p.video_codec    = AV_CODEC_ID_MPEG4,
    .p.subtitle_codec = AV_CODEC_ID_NONE,
    .p.flags          = AVFMT_VARIABLE_FPS,
    .priv_data_size   = sizeof(DynacolorMuxContext),
    .init             = dynacolor_init,
    .write_packet     = dynacolor_write_packet,
    .check_bitstream  = dynacolor_check_bitstream,
};
