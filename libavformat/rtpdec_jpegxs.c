/*
 * RTP parser for JPEG XS payload format (RFC 9134)
 * Copyright (c) 2026
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

/**
 * @file
 * @brief JPEG XS (ISO/IEC 21122) RTP depacketization code (RFC 9134)
 *
 * Both the codestream packetization mode (K == 0, one packetization
 * unit per JPEG XS picture segment) and the slice packetization mode
 * (K == 1, one packetization unit per slice) are supported for
 * sequential transmission. Out-of-order transmission (T == 0) is not
 * supported and such packets are rejected.
 *
 * A picture segment on the wire is the concatenation of an ISO video
 * support box, a color specification box (both defined in ISO/IEC
 * 21122-3) and the JPEG XS codestream. The FFmpeg JPEG XS decode path
 * expects one bare codestream (SOC marker through EOC marker) per
 * packet, so the leading ISO boxes are skipped generically by walking
 * the box structure until the SOC marker is found. One packet per
 * picture segment is emitted; for interlaced streams, each field
 * therefore becomes its own packet with the same timestamp.
 */

#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/parseutils.h"

#include "libavcodec/jpegxs.h"

#include "avformat.h"
#include "rtpdec.h"
#include "rtpdec_formats.h"

#define RTP_JPEGXS_PAYLOAD_HEADER_SIZE 4

/* SEP counter value marking the JPEG XS header segment in slice
 * packetization mode (RFC 9134, Section 4.3) */
#define RTP_JPEGXS_SEP_HDR_SEGMENT     0x7ff

#define RTP_JPEGXS_MAX_DIMENSION       32767

struct PayloadContext {
    int packetmode;         ///< configured K bit from the SDP (0/1)
    int transmode;          ///< configured T bit from the SDP (default 1)

    /* frame reassembly state */
    AVIOContext *frame;     ///< current (partial) picture segment
    uint32_t timestamp;     ///< RTP timestamp of the current frame
    uint16_t expected_seq;  ///< sequence number of the previous packet
    int seq_valid;
    int strip_boxes;        ///< skip ISO boxes at the current write position
};

static av_cold int jpegxs_sdp_parse_fmtp_config(AVFormatContext *s,
                                                AVStream *stream,
                                                PayloadContext *jxs_data,
                                                const char *attr,
                                                const char *value)
{
    AVCodecParameters *par = stream->codecpar;

    /* required parameter (RFC 9134, Section 7.1) */
    if (!strcmp(attr, "packetmode")) {
        int v = atoi(value);
        if (v != 0 && v != 1) {
            av_log(s, AV_LOG_ERROR, "Invalid JPEG XS packetmode %s\n", value);
            return AVERROR_INVALIDDATA;
        }
        jxs_data->packetmode = v;
    } else if (!strcmp(attr, "transmode")) {
        int v = atoi(value);
        if (v != 0 && v != 1) {
            av_log(s, AV_LOG_ERROR, "Invalid JPEG XS transmode %s\n", value);
            return AVERROR_INVALIDDATA;
        }
        jxs_data->transmode = v;
    } else if (!strcmp(attr, "width") || !strcmp(attr, "height")) {
        int v = atoi(value);
        if (v < 1 || v > RTP_JPEGXS_MAX_DIMENSION) {
            av_log(s, AV_LOG_ERROR, "Invalid JPEG XS %s %s\n", attr, value);
            return AVERROR_INVALIDDATA;
        }
        if (attr[0] == 'w')
            par->width = v;
        else
            par->height = v;
    } else if (!strcmp(attr, "depth")) {
        int v = atoi(value);
        if (v <= 0 || v > 16) {
            av_log(s, AV_LOG_ERROR, "Invalid JPEG XS depth %s\n", value);
            return AVERROR_INVALIDDATA;
        }
        par->bits_per_raw_sample = v;
    } else if (!strcmp(attr, "exactframerate")) {
        AVRational framerate;
        if (av_parse_video_rate(&framerate, value) >= 0)
            stream->avg_frame_rate = framerate;
        else
            av_log(s, AV_LOG_WARNING, "Invalid JPEG XS exactframerate %s\n",
                   value);
    } else if (!strcmp(attr, "interlace")) {
        par->field_order = AV_FIELD_TT;
    }

    /* profile, level, sublevel, sampling, colorimetry, TCS, RANGE and
     * segmented are declarative duplicates of in-band information
     * (RFC 9134, Section 7): the values from the payload data prevail,
     * so they are intentionally not interpreted here */

    return 0;
}

static av_cold int jpegxs_parse_sdp_line(AVFormatContext *ctx, int st_index,
                                         PayloadContext *jxs_data,
                                         const char *line)
{
    const char *sdp_line_ptr = line;

    if (st_index < 0)
        return 0;

    if (av_strstart(sdp_line_ptr, "fmtp:", &sdp_line_ptr))
        return ff_parse_fmtp(ctx, ctx->streams[st_index], jxs_data,
                             sdp_line_ptr, jpegxs_sdp_parse_fmtp_config);

    return 0;
}

static void jpegxs_reset_frame(PayloadContext *rtp_jxs_ctx)
{
    uint8_t *dummy;

    if (rtp_jxs_ctx->frame) {
        avio_close_dyn_buf(rtp_jxs_ctx->frame, &dummy);
        av_free(dummy);
        rtp_jxs_ctx->frame = NULL;
    }
    rtp_jxs_ctx->strip_boxes = 0;
}

/* Skip the ISO box chain (video support box and color specification
 * box, ISO/IEC 21122-3) preceding the codestream and return the offset
 * of the SOC marker, a negative error if the data is not plausible, or
 * len if the box chain extends beyond this payload. */
static int jpegxs_skip_boxes(AVFormatContext *ctx, const uint8_t *buf, int len)
{
    int pos = 0;

    while (pos + 2 <= len) {
        uint32_t box_size;

        if (AV_RB16(buf + pos) == JPEGXS_MARKER_SOC)
            return pos;

        /* generic ISO base media box: 32-bit size followed by a
         * four-character type */
        if (pos + 8 > len)
            return len;
        box_size = AV_RB32(buf + pos);
        if (box_size < 8 || box_size > (uint32_t)(len - pos)) {
            /* boxes small enough to straddle a packet boundary are not
             * expected: the header segment is required to fit its
             * packetization unit */
            av_log(ctx, AV_LOG_ERROR,
                   "Invalid ISO box (size %"PRIu32") before JPEG XS "
                   "codestream\n", box_size);
            return AVERROR_INVALIDDATA;
        }
        pos += box_size;
    }

    return len;
}

static int jpegxs_handle_packet(AVFormatContext *ctx,
                                PayloadContext *rtp_jxs_ctx,
                                AVStream *st, AVPacket *pkt,
                                uint32_t *timestamp, const uint8_t *buf,
                                int len, uint16_t seq, int flags)
{
    int t_bit, k_bit, l_bit, i_bits, f_counter, sep_counter, p_counter;
    int pu_start, produced, res;

    if (len < RTP_JPEGXS_PAYLOAD_HEADER_SIZE) {
        av_log(ctx, AV_LOG_ERROR,
               "Too short RTP/JPEG XS packet, got %d bytes\n", len);
        return AVERROR_INVALIDDATA;
    }

    /*
     * decode the payload header (RFC 9134, Section 4.3):
     *
     *     0                   1                   2                   3
     *     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *    |T|K|L| I |F counter|     SEP counter     |     P counter       |
     *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     */
    t_bit       =  buf[0] >> 7;
    k_bit       = (buf[0] >> 6) & 0x01;
    l_bit       = (buf[0] >> 5) & 0x01;
    i_bits      = (buf[0] >> 3) & 0x03;
    f_counter   = ((buf[0] & 0x07) << 2) | (buf[1] >> 6);
    sep_counter = ((buf[1] & 0x3f) << 5) | (buf[2] >> 3);
    p_counter   = ((buf[2] & 0x07) << 8) | buf[3];

    av_log(ctx, AV_LOG_TRACE,
           "RTP/JPEG XS packet: T %d, K %d, L %d, I %d, F %d, SEP %d, P %d, "
           "M %d, %d bytes\n", t_bit, k_bit, l_bit, i_bits, f_counter,
           sep_counter, p_counter, !!(flags & RTP_FLAG_MARKER), len);

    if (!t_bit) {
        /* only sequential transmission is supported; reordering
         * out-of-order slice mode streams is not implemented */
        avpriv_report_missing_feature(ctx,
                                      "Out-of-order RTP/JPEG XS transmission");
        return AVERROR_PATCHWELCOME;
    }

    if (i_bits == 0x01) {
        av_log(ctx, AV_LOG_ERROR, "Reserved RTP/JPEG XS I field value\n");
        return AVERROR_INVALIDDATA;
    }

    buf += RTP_JPEGXS_PAYLOAD_HEADER_SIZE;
    len -= RTP_JPEGXS_PAYLOAD_HEADER_SIZE;

    /* a packetization unit starts whenever the P counter (extended by
     * the SEP counter in codestream mode) is zero */
    pu_start = p_counter == 0 && (k_bit == 1 || sep_counter == 0);

    if (rtp_jxs_ctx->frame) {
        if (rtp_jxs_ctx->seq_valid &&
            seq != (uint16_t)(rtp_jxs_ctx->expected_seq + 1)) {
            av_log(ctx, AV_LOG_WARNING,
                   "Lost RTP packet within a JPEG XS frame, dropping it\n");
            jpegxs_reset_frame(rtp_jxs_ctx);
        } else if (rtp_jxs_ctx->timestamp != *timestamp) {
            av_log(ctx, AV_LOG_WARNING,
                   "RTP/JPEG XS timestamp changed mid-frame, dropping it\n");
            jpegxs_reset_frame(rtp_jxs_ctx);
        }
    }

    rtp_jxs_ctx->expected_seq = seq;
    rtp_jxs_ctx->seq_valid    = 1;

    /* in slice packetization mode, a header segment (identified by an
     * SEP counter of 0x7ff) marks the start of a picture segment; if
     * one arrives while a previous picture segment is still buffered
     * (the first field of an interlaced frame, which carries no L bit
     * or marker of its own), that previous segment is complete and
     * emitted now, while the current payload is buffered for the next
     * one within the same call */
    produced = 0;
    if (rtp_jxs_ctx->frame && pu_start && k_bit == 1 &&
        sep_counter == RTP_JPEGXS_SEP_HDR_SEGMENT) {
        res = ff_rtp_finalize_packet(pkt, &rtp_jxs_ctx->frame, st->index);
        if (res < 0)
            return res;
        pkt->flags |= AV_PKT_FLAG_KEY;
        produced = 1;
    }

    if (!rtp_jxs_ctx->frame) {
        /* a picture segment can only start at a packetization unit
         * boundary (and, in slice packetization mode, only at the
         * header segment); wait for one after a loss or at startup */
        if (!pu_start ||
            (k_bit == 1 && sep_counter != RTP_JPEGXS_SEP_HDR_SEGMENT)) {
            av_log(ctx, AV_LOG_TRACE,
                   "Waiting for the start of a JPEG XS picture segment\n");
            return produced ? 0 : AVERROR(EAGAIN);
        }
        if ((res = avio_open_dyn_buf(&rtp_jxs_ctx->frame)) < 0)
            return res;
        rtp_jxs_ctx->timestamp = *timestamp;
        /* the picture segment starts with the VS and CS boxes, which
         * the FFmpeg JPEG XS decode path does not expect */
        rtp_jxs_ctx->strip_boxes = 1;
    }

    if (rtp_jxs_ctx->strip_boxes) {
        int off = jpegxs_skip_boxes(ctx, buf, len);
        if (off < 0) {
            jpegxs_reset_frame(rtp_jxs_ctx);
            return produced ? 0 : off;
        }
        if (off < len)
            rtp_jxs_ctx->strip_boxes = 0;
        buf += off;
        len -= off;
    }

    avio_write(rtp_jxs_ctx->frame, buf, len);

    /* emit one packet per picture segment: in codestream packetization
     * mode a picture segment ends with its packetization unit (L == 1),
     * in slice packetization mode with the end of the frame/field as
     * indicated by the RTP marker bit (RFC 9134, Sections 4.1-4.3) */
    if ((k_bit == 0 && l_bit) || (k_bit == 1 && (flags & RTP_FLAG_MARKER))) {
        if (produced) {
            /* a header segment packetization unit cannot also terminate
             * a picture segment; drop the malformed segment, the packet
             * finalized above is still delivered */
            av_log(ctx, AV_LOG_WARNING,
                   "Malformed RTP/JPEG XS header segment terminating a "
                   "picture segment, dropping it\n");
            jpegxs_reset_frame(rtp_jxs_ctx);
            return 0;
        }
        res = ff_rtp_finalize_packet(pkt, &rtp_jxs_ctx->frame, st->index);
        rtp_jxs_ctx->strip_boxes = 0;
        if (res < 0)
            return res;
        /* JPEG XS is intra-only; every frame is a sync point */
        pkt->flags |= AV_PKT_FLAG_KEY;
        return 0;
    }

    return produced ? 0 : AVERROR(EAGAIN);
}

static av_cold void jpegxs_close_context(PayloadContext *jxs_data)
{
    jpegxs_reset_frame(jxs_data);
}

const RTPDynamicProtocolHandler ff_jpegxs_dynamic_handler = {
    .enc_name         = "jxsv",
    .codec_type       = AVMEDIA_TYPE_VIDEO,
    .codec_id         = AV_CODEC_ID_JPEGXS,
    .need_parsing     = AVSTREAM_PARSE_FULL,
    .priv_data_size   = sizeof(PayloadContext),
    .parse_sdp_a_line = jpegxs_parse_sdp_line,
    .close            = jpegxs_close_context,
    .parse_packet     = jpegxs_handle_packet,
};
