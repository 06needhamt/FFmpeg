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

#include "libavutil/attributes.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"

#include "libavcodec/defs.h"
#include "libavcodec/get_bits.h"
#include "libavcodec/golomb.h"
#include "libavcodec/h264.h"
#include "libavcodec/startcode.h"

#include "avformat.h"
#include "dynacolor.h"
#include "internal.h"
#include "mux.h"

#define H264_MAX_SPS 32
#define H264_MAX_PPS 256

typedef struct H264ParamSet {
    uint8_t *data; ///< NAL unit, without start code
    int      size;
    int      sps_id;
    int      pending; ///< current definition not yet written
} H264ParamSet;

typedef struct H264Unit {
    const uint8_t *nal;  ///< NAL unit header
    int            size; ///< NAL unit size
    const uint8_t *src;  ///< Annex B input: bytes written as is, start code included
    int            src_size;
    int            type;
} H264Unit;

typedef struct DynacolorMuxStream {
    int     format;
    int     pal;
    int     width_base;
    int64_t last_time;

    /* H.264: input NAL unit length size (0 for Annex B), and the current
     * parameter sets, inserted in key access units that lack them */
    int           nal_length_size;
    H264ParamSet  sps[H264_MAX_SPS];
    H264ParamSet  pps[H264_MAX_PPS];
    H264Unit     *units;
    unsigned      units_size;
    int           slice_pps_warned;

    /* MPEG-4: the current configuration headers up to the VOL, inserted in
     * keyframes that lack them */
    uint8_t      *mpeg4_config;
    int           mpeg4_config_size;
} DynacolorMuxStream;

typedef struct DynacolorMuxContext {
    int64_t  start_time;
    uint32_t serial;
} DynacolorMuxContext;

/* PES header bytes 6-23 as written by Dynacolor devices: PES flags, header
 * data length, PTS and DTS flagged as absent with value 0, and the PES
 * extension with no previous record size. */
static const uint8_t pes_filler[18] = {
    0x87, 0x01, 0x17, 0x31, 0x00, 0x01, 0x00, 0x01, 0x11,
    0x00, 0x01, 0x00, 0x01, 0x0F, 0x8B, 0x00, 0x00, 0x00,
};

typedef struct H264NAL {
    const uint8_t *start;   ///< start code
    const uint8_t *header;  ///< NAL unit header
    int type;
} H264NAL;

/* Find the next H.264 NAL unit at or after p. */
static int h264_next_nal(const uint8_t *p, const uint8_t *buf, const uint8_t *end,
                         H264NAL *nal)
{
    uint32_t state = -1;

    p = avpriv_find_start_code(p, end, &state);
    if ((state & 0xFFFFFF00) != 0x100)
        return 0;
    nal->header = p - 1;
    nal->start  = p - 4;
    if (nal->start > buf && !nal->start[-1])
        nal->start--;
    nal->type   = *nal->header & 0x1F;
    return 1;
}

/* Read the first Exp-Golomb values of the H.264 NAL unit starting at nal,
 * after skip bits of its payload, ignoring emulation prevention bytes. */
static void h264_read_ue(const uint8_t *nal, const uint8_t *end, int skip,
                         unsigned *val, int nb_val)
{
    uint8_t rbsp[32 + AV_INPUT_BUFFER_PADDING_SIZE] = { 0 };
    const uint8_t *p = nal + 1;
    int len = 0, zeros = 0;
    GetBitContext gb;

    for (; p < end && len < 32; p++) {
        if (zeros >= 2 && *p == 3) {
            zeros = 0;
            continue;
        }
        zeros = *p ? 0 : zeros + 1;
        rbsp[len++] = *p;
    }
    init_get_bits8(&gb, rbsp, len);
    skip_bits_long(&gb, skip);
    for (int i = 0; i < nb_val; i++)
        val[i] = get_ue_golomb_long(&gb);
}

/**
 * Find the type of the first picture of an MPEG-4 packet, and the
 * configuration headers before it.
 *
 * @param config      set to the offset of the configuration headers
 * @param config_size set to their size, up to the first GOV or VOP header,
 *                    or to 0 if the packet carries no VOL header
 * @return DYNACOLOR_PICTURE_I/P/B, or 0 if no picture was found
 */
static int mpeg4_parse_packet(const uint8_t *buf, int size,
                              int *config, int *config_size)
{
    const uint8_t *p = buf, *end = buf + size, *start = NULL;
    uint32_t state = -1;
    int vol = 0;

    *config = *config_size = 0;
    while ((p = avpriv_find_start_code(p, end, &state)) < end) {
        if (state != 0x1B3 && state != 0x1B6) {
            if (!start)
                start = p - 4;
            vol |= state >= 0x120 && state <= 0x12F;
            continue;
        }
        if (vol) {
            *config      = start - buf;
            *config_size = p - 4 - start;
            vol = 0;
        }
        if (state == 0x1B6) {
            switch (*p >> 6) {
            case 0:  return DYNACOLOR_PICTURE_I;
            case 2:  return DYNACOLOR_PICTURE_B;
            default: return DYNACOLOR_PICTURE_P;
            }
        }
    }
    if (vol) {
        *config      = start - buf;
        *config_size = end - start;
    }
    return 0;
}

/* Make data the current MPEG-4 configuration. */
static int mpeg4_set_config(DynacolorMuxStream *dst, const uint8_t *data, size_t size)
{
    if (size > INT_MAX)
        return AVERROR(EINVAL);
    if (size == dst->mpeg4_config_size &&
        (!size || !memcmp(data, dst->mpeg4_config, size)))
        return 0;
    av_freep(&dst->mpeg4_config);
    dst->mpeg4_config_size = 0;
    if (!size)
        return 0;
    dst->mpeg4_config = av_memdup(data, size);
    if (!dst->mpeg4_config)
        return AVERROR(ENOMEM);
    dst->mpeg4_config_size = size;
    return 0;
}

/* Make an SPS or PPS the current one for its id. */
static int h264_store_param_set(DynacolorMuxStream *dst, const uint8_t *nal,
                                int size, int pending)
{
    H264ParamSet *ps;
    uint8_t *data;
    unsigned val[2];
    int sps_id = 0;

    while (size > 1 && !nal[size - 1])
        size--;
    if (size < 2)
        return 0;

    if ((nal[0] & 0x1F) == H264_NAL_SPS) {
        h264_read_ue(nal, nal + size, 24, val, 1);
        if (val[0] >= H264_MAX_SPS)
            return 0;
        ps = &dst->sps[val[0]];
    } else {
        h264_read_ue(nal, nal + size, 0, val, 2);
        if (val[0] >= H264_MAX_PPS || val[1] >= H264_MAX_SPS)
            return 0;
        ps = &dst->pps[val[0]];
        sps_id = val[1];
    }

    if (!pending && ps->size == size && !memcmp(ps->data, nal, size)) {
        ps->pending = 0;
        return 0;
    }
    data = av_memdup(nal, size);
    if (!data)
        return AVERROR(ENOMEM);
    av_free(ps->data);
    ps->data    = data;
    ps->size    = size;
    ps->sps_id  = sps_id;
    ps->pending = pending;
    return 0;
}

/* Update the current parameter sets from avcC or Annex B extradata. */
static int h264_parse_extradata(AVFormatContext *s, DynacolorMuxStream *dst,
                                const uint8_t *buf, int size)
{
    const uint8_t *end = buf + size;
    H264NAL nal, next;
    int ret;

    if (size >= 7 && buf[0] == 1) {
        const uint8_t *p = buf + 5;

        dst->nal_length_size = (buf[4] & 3) + 1;
        /* SPS, then PPS */
        for (int list = 0; list < 2 && p < end; list++) {
            int nb = *p++ & (list ? 0xFF : 0x1F);
            for (int i = 0; i < nb; i++) {
                int nal_size;
                if (end - p < 2 || end - p - 2 < AV_RB16(p))
                    goto invalid;
                nal_size = AV_RB16(p);
                p += 2;
                if (nal_size && ((*p & 0x1F) == H264_NAL_SPS || (*p & 0x1F) == H264_NAL_PPS)) {
                    ret = h264_store_param_set(dst, p, nal_size, 1);
                    if (ret < 0)
                        return ret;
                }
                p += nal_size;
            }
        }
        return 0;
    }

    if (!h264_next_nal(buf, buf, end, &nal))
        return 0;
    dst->nal_length_size = 0;
    for (;; nal = next) {
        int next_have = h264_next_nal(nal.header + 1, buf, end, &next);

        if (nal.type == H264_NAL_SPS || nal.type == H264_NAL_PPS) {
            ret = h264_store_param_set(dst, nal.header,
                                       (next_have ? next.start : end) - nal.header, 1);
            if (ret < 0)
                return ret;
        }
        if (!next_have)
            break;
    }
    return 0;

invalid:
    av_log(s, AV_LOG_WARNING, "Invalid H.264 extradata\n");
    return 0;
}

static int h264_add_unit(DynacolorMuxStream *dst, int *nb_units,
                         const uint8_t *nal, int size, const uint8_t *src, int src_size)
{
    H264Unit *unit = av_fast_realloc(dst->units, &dst->units_size,
                                     (*nb_units + 1) * sizeof(*unit));
    if (!unit)
        return AVERROR(ENOMEM);
    dst->units = unit;
    unit += (*nb_units)++;
    unit->nal      = nal;
    unit->size     = size;
    unit->type     = *nal & 0x1F;
    unit->src      = src;
    unit->src_size = src_size;
    return 0;
}

/* Split an H.264 packet into NAL units. */
static int h264_split_packet(AVFormatContext *s, DynacolorMuxStream *dst,
                             const AVPacket *pkt, int *nb_units)
{
    const uint8_t *p = pkt->data, *end = pkt->data + pkt->size;
    H264NAL nal, next;
    int ret;

    *nb_units = 0;

    if (dst->nal_length_size) {
        while (end - p >= dst->nal_length_size) {
            uint32_t nal_size = 0;
            for (int i = 0; i < dst->nal_length_size; i++)
                nal_size = nal_size << 8 | *p++;
            if (nal_size > end - p)
                return AVERROR_INVALIDDATA;
            if (nal_size) {
                ret = h264_add_unit(dst, nb_units, p, nal_size, NULL, 0);
                if (ret < 0)
                    return ret;
            }
            p += nal_size;
        }
        return p == end ? 0 : AVERROR_INVALIDDATA;
    }

    if (!h264_next_nal(p, p, end, &nal)) {
        av_log(s, AV_LOG_ERROR, "H.264 packet without start code, and no avcC configuration\n");
        return AVERROR_INVALIDDATA;
    }
    /* Annex B NAL units are written as they are, with their start codes and
     * any leading or trailing zero bytes. */
    for (;;) {
        int has_next = h264_next_nal(nal.header + 1, pkt->data, end, &next);
        const uint8_t *nal_end = has_next ? next.start : end;
        const uint8_t *src = *nb_units ? nal.start : pkt->data;

        ret = h264_add_unit(dst, nb_units, nal.header, nal_end - nal.header,
                            src, nal_end - src);
        if (ret < 0)
            return ret;
        if (!has_next)
            return 0;
        nal = next;
    }
}

static int h264_has_slice_header(int nal_type)
{
    return nal_type == H264_NAL_SLICE || nal_type == H264_NAL_IDR_SLICE ||
           nal_type == H264_NAL_DPA;
}

/* Find the picture type of a split H.264 access unit. */
static int h264_picture_type(const H264Unit *units, int nb_units)
{
    for (int i = 0; i < nb_units; i++) {
        if (h264_has_slice_header(units[i].type)) {
            unsigned val[2]; /* first_mb_in_slice, slice_type */
            h264_read_ue(units[i].nal, units[i].nal + units[i].size, 0, val, 2);
            switch (val[1] % 5) {
            case 1:  return DYNACOLOR_PICTURE_B;
            case 2:
            case 4:  return DYNACOLOR_PICTURE_I;
            default: return DYNACOLOR_PICTURE_P;
            }
        }
    }
    return 0;
}

/**
 * Warn, once per stream, when a slice after the first one refers to a PPS
 * that the access unit does not carry. Only the parameter sets of the first
 * slice are inserted, so decoding may fail at such an access unit.
 */
static void h264_check_slice_pps(AVFormatContext *s, int stream_index,
                                 DynacolorMuxStream *dst, const H264Unit *units,
                                 int nb_units, int key)
{
    uint64_t pps_mask[4] = { 0 };
    unsigned val[3];
    int first = -1;

    if (dst->slice_pps_warned)
        return;

    for (int i = 0; i < nb_units; i++) {
        const H264Unit *u = &units[i];
        if (u->type == H264_NAL_PPS) {
            h264_read_ue(u->nal, u->nal + u->size, 0, val, 1);
            if (val[0] < H264_MAX_PPS)
                pps_mask[val[0] >> 6] |= 1ULL << (val[0] & 63);
        } else if (h264_has_slice_header(u->type)) {
            /* first_mb_in_slice, slice_type, pic_parameter_set_id */
            h264_read_ue(u->nal, u->nal + u->size, 0, val, 3);
            if (val[2] >= H264_MAX_PPS)
                continue;
            if (first < 0) {
                first = val[2];
            } else if (val[2] != first && !(pps_mask[val[2] >> 6] & 1ULL << (val[2] & 63)) &&
                       dst->pps[val[2]].data && (key || dst->pps[val[2]].pending)) {
                av_log(s, AV_LOG_WARNING, "Stream %d: a slice refers to PPS %u, "
                       "which is not inserted as it is not the PPS of the first "
                       "slice; decoding may fail at this access unit\n",
                       stream_index, val[2]);
                dst->slice_pps_warned = 1;
                return;
            }
        }
    }
}

typedef struct H264ConfigInsert {
    int           before[2]; ///< index of the unit to insert before
    H264ParamSet *ps[2];     ///< SPS first
    int           nb;
} H264ConfigInsert;

/**
 * Find the current SPS and PPS that the first slice refers to but does not
 * carry. Repeat them at key access units; otherwise only insert definitions
 * that have not been written. The SPS goes at the start of the access unit,
 * after its AUD if there is one, so that an in-band PPS can refer to it.
 * The PPS goes immediately before the first slice, after the in-band SPS.
 */
static void h264_find_config_insert(DynacolorMuxStream *dst,
                                    const H264Unit *units, int nb_units, int key,
                                    H264ConfigInsert *ins)
{
    uint32_t sps_mask = 0, sps_needed_early = 0;
    uint64_t pps_mask[4] = { 0 };
    uint8_t pps_sps[H264_MAX_PPS];
    unsigned val[3], pps_id, sps_id;
    int slice;

    memset(ins, 0, sizeof(*ins));

    for (slice = 0; slice < nb_units; slice++) {
        const H264Unit *u = &units[slice];
        if (h264_has_slice_header(u->type))
            break;
        if (u->type == H264_NAL_SPS) {
            h264_read_ue(u->nal, u->nal + u->size, 24, val, 1);
            if (val[0] < H264_MAX_SPS)
                sps_mask |= 1U << val[0];
        } else if (u->type == H264_NAL_PPS) {
            h264_read_ue(u->nal, u->nal + u->size, 0, val, 2);
            if (val[0] < H264_MAX_PPS && val[1] < H264_MAX_SPS) {
                pps_mask[val[0] >> 6] |= 1ULL << (val[0] & 63);
                pps_sps[val[0]] = val[1];
                sps_needed_early |= (1U << val[1]) & ~sps_mask;
            }
        }
    }
    if (slice == nb_units)
        return;

    /* first_mb_in_slice, slice_type, pic_parameter_set_id */
    h264_read_ue(units[slice].nal, units[slice].nal + units[slice].size, 0, val, 3);
    pps_id = val[2];
    if (pps_id >= H264_MAX_PPS)
        return;

    if (pps_mask[pps_id >> 6] & 1ULL << (pps_id & 63)) {
        sps_id = pps_sps[pps_id];
    } else {
        if (!dst->pps[pps_id].data)
            return;
        sps_id = dst->pps[pps_id].sps_id;
    }

    /* An in-band SPS after a PPS is too late for parsing that PPS. */
    sps_mask &= ~sps_needed_early;
    if (!(sps_mask & 1U << sps_id) && dst->sps[sps_id].data &&
        (key || dst->sps[sps_id].pending)) {
        ins->before[ins->nb] = nb_units && units[0].type == H264_NAL_AUD;
        ins->ps[ins->nb++]   = &dst->sps[sps_id];
    }
    if (!(pps_mask[pps_id >> 6] & 1ULL << (pps_id & 63)) &&
        (key || dst->pps[pps_id].pending)) {
        ins->before[ins->nb] = slice;
        ins->ps[ins->nb++]   = &dst->pps[pps_id];
    }
}

/* Write an H.264 NAL unit with a start code, or count its size if pb is NULL. */
static int h264_write_nal(AVIOContext *pb, const uint8_t *nal, int size,
                          int start_code_size)
{
    if (pb) {
        if (start_code_size == 4)
            avio_wb32(pb, 1);
        else
            avio_wb24(pb, 1);
        avio_write(pb, nal, size);
    }
    return start_code_size + size;
}

/* Write a split H.264 access unit in Annex B form with the inserted parameter
 * sets, or count its size if pb is NULL. The conversion of length-prefixed
 * NAL units is done here, with the start codes h264_mp4toannexb uses. */
static int64_t h264_write_units(AVIOContext *pb, const H264Unit *units, int nb_units,
                                const H264ConfigInsert *ins)
{
    int64_t size = 0;

    for (int i = 0, j = 0; i <= nb_units; i++) {
        for (; j < ins->nb && ins->before[j] == i; j++)
            size += h264_write_nal(pb, ins->ps[j]->data, ins->ps[j]->size, 4);
        if (i == nb_units)
            break;
        if (units[i].src) {
            if (pb)
                avio_write(pb, units[i].src, units[i].src_size);
            size += units[i].src_size;
        } else {
            int ps = units[i].type == H264_NAL_SPS || units[i].type == H264_NAL_PPS;
            size += h264_write_nal(pb, units[i].nal, units[i].size,
                                   ps || !size ? 4 : 3);
        }
    }
    return size;
}

static av_cold int dynacolor_init(AVFormatContext *s)
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
        /* The demuxer can only place 30 records per second of a channel. */
        if (!dst->pal && av_cmp_q(rate, (AVRational){ 30, 1 }) > 0)
            av_log(s, AV_LOG_WARNING, "Stream %u: frame rates above 30 fps cannot be "
                   "represented, timestamps will not be preserved\n", i);

        switch (par->width) {
        case 704: dst->width_base = 1; break;
        case 640: dst->width_base = 2; break;
        default:  dst->width_base = 0; break;
        }

        if (par->codec_id == AV_CODEC_ID_H264 && par->extradata_size) {
            ret = h264_parse_extradata(s, dst, par->extradata, par->extradata_size);
            if (ret < 0)
                return ret;
        } else if (par->codec_id == AV_CODEC_ID_MPEG4) {
            ret = mpeg4_set_config(dst, par->extradata, par->extradata_size);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
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
    /* Channels are numbered by stream index, not by stream id. */
    int channel    = pkt->stream_index;
    int64_t time, data_size = pkt->size;
    H264ConfigInsert ins = { 0 };
    int nb_units = 0, pic_type = 0, extra_size = 0, config, config_size, ret;
    uint32_t size;
    size_t side_size;
    const uint8_t *side = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA,
                                                  &side_size);

    if (dst->format == DYNACOLOR_FORMAT_H264) {
        if (side && side_size <= INT_MAX) {
            ret = h264_parse_extradata(s, dst, side, side_size);
            if (ret < 0)
                return ret;
        }

        ret = h264_split_packet(s, dst, pkt, &nb_units);
        if (ret < 0)
            return ret;

        h264_find_config_insert(dst, dst->units, nb_units,
                                pkt->flags & AV_PKT_FLAG_KEY, &ins);
        h264_check_slice_pps(s, pkt->stream_index, dst, dst->units, nb_units,
                             pkt->flags & AV_PKT_FLAG_KEY);
        data_size = h264_write_units(NULL, dst->units, nb_units, &ins);
        pic_type  = h264_picture_type(dst->units, nb_units);
    } else if (dst->format == DYNACOLOR_FORMAT_MPEG4) {
        if (side) {
            ret = mpeg4_set_config(dst, side, side_size);
            if (ret < 0)
                return ret;
        }
        /* In-band configuration headers replace the current ones. */
        pic_type = mpeg4_parse_packet(pkt->data, pkt->size, &config, &config_size);
        if (config_size) {
            ret = mpeg4_set_config(dst, pkt->data + config, config_size);
            if (ret < 0)
                return ret;
        } else if (side || ((pkt->flags & AV_PKT_FLAG_KEY) && (s->flags & AVFMT_FLAG_AUTO_BSF))) {
            extra_size = dst->mpeg4_config_size;
            data_size += extra_size;
        }
    } else {
        pic_type  = DYNACOLOR_PICTURE_I;
    }
    if (!pic_type)
        pic_type = pkt->flags & AV_PKT_FLAG_KEY ? DYNACOLOR_PICTURE_I : DYNACOLOR_PICTURE_P;

    if (data_size > DYNACOLOR_MAX_RECORD_SIZE - DYNACOLOR_PES_HEADER_SIZE) {
        av_log(s, AV_LOG_ERROR, "Packet of size %"PRId64" is too large\n", data_size);
        return AVERROR(EINVAL);
    }
    size = data_size + DYNACOLOR_PES_HEADER_SIZE;

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

    memset(buf, 0, sizeof(buf));

    basic[0]  = '@';
    basic[1]  = '2';
    /* Channels above 31 need the channel extension bits, enabled by bit 7. */
    basic[2]  = dst->width_base << 5 | (channel >> 5 ? 0x80 : 0);
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
    /* As written by Dynacolor devices, modulo 2^16 for large records. */
    AV_WB16(pes + 4, (size - 6) & 0xFFFF);
    memcpy(pes + 6, pes_filler, sizeof(pes_filler));
    pes[24] = 0x80 | (size >> 15 & 0x7F);
    pes[25] = (size >> 11 & 0xF) << 4 | 0x08 | (size >> 8 & 0x7);
    pes[26] = size & 0xFF;
    pes[27] = pic_type;
    AV_WN32(pes + 28, 0xFFFFFFFF);

    avio_write(pb, buf, sizeof(buf));
    if (dst->format == DYNACOLOR_FORMAT_H264) {
        h264_write_units(pb, dst->units, nb_units, &ins);
        for (int i = 0; i < ins.nb; i++)
            ins.ps[i]->pending = 0;

        /* In-band parameter sets replace the current ones, in decoding order. */
        for (int i = 0; i < nb_units; i++) {
            if (dst->units[i].type == H264_NAL_SPS || dst->units[i].type == H264_NAL_PPS) {
                ret = h264_store_param_set(dst, dst->units[i].nal, dst->units[i].size, 0);
                if (ret < 0)
                    return ret;
            }
        }
    } else {
        if (extra_size)
            avio_write(pb, dst->mpeg4_config, extra_size);
        avio_write(pb, pkt->data, pkt->size);
    }

    return 0;
}

static av_cold void dynacolor_deinit(AVFormatContext *s)
{
    for (unsigned i = 0; i < s->nb_streams; i++) {
        DynacolorMuxStream *dst = s->streams[i]->priv_data;
        if (!dst)
            continue;
        for (int j = 0; j < H264_MAX_SPS; j++)
            av_freep(&dst->sps[j].data);
        for (int j = 0; j < H264_MAX_PPS; j++)
            av_freep(&dst->pps[j].data);
        av_freep(&dst->units);
        av_freep(&dst->mpeg4_config);
    }
}

static int dynacolor_query_codec(enum AVCodecID id, int std_compliance)
{
    return id == AV_CODEC_ID_MJPEG || id == AV_CODEC_ID_MPEG4 ||
           id == AV_CODEC_ID_H264;
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
    .deinit           = dynacolor_deinit,
    .query_codec      = dynacolor_query_codec,
};
