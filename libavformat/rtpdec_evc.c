/*
 * RTP parser for EVC payload format (RFC 9584)
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
 * @brief EVC (ISO/IEC 23094-1) RTP depacketization code (RFC 9584)
 *
 * The de-packetization process turns the three RFC 9584 payload
 * structures (single NAL unit packets, aggregation packets with
 * Type == 56 and fragmentation units with Type == 57) back into a
 * length-prefixed NAL unit stream. Unlike the Annex B based H.26x
 * codecs, EVC natively uses NAL units prefixed with a four-byte
 * length field (ISO/IEC 23094-1, Annex B / EVC_NALU_LENGTH_PREFIX_SIZE),
 * which is the framing the evc parser and decoder expect.
 *
 * Note that the Type field of the RFC 9584 payload header carries
 * nal_unit_type_plus1, i.e. the actual NAL unit type plus one, and a
 * value of zero is therefore illegal on the wire.
 */

#include "libavutil/avstring.h"
#include "libavutil/base64.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "libavcodec/evc.h"

#include "avformat.h"
#include "internal.h"
#include "rtpdec.h"
#include "rtpdec_formats.h"

#define RTP_EVC_PAYLOAD_HEADER_SIZE       2
#define RTP_EVC_FU_HEADER_SIZE            1
#define RTP_EVC_DONL_FIELD_SIZE           2
#define RTP_EVC_AP_NALU_LENGTH_FIELD_SIZE 2

/* RFC 9584, Section 4.3.2 / 4.3.3: values of the Type field
 * (nal_unit_type_plus1 numbering space) */
#define RTP_EVC_AP_TYPE                   56
#define RTP_EVC_FU_TYPE                   57

/* Highest Type value of a real NAL unit that may be passed to the
 * decoder (RFC 9584, Section 6: values 1..55 may be passed on,
 * NAL-unit-like structures 56..62 must not). */
#define RTP_EVC_MAX_REAL_NAL_TYPE         55

struct PayloadContext {
    int using_donl_field;
    int profile_id;
    int level_id;

    /* out-of-band parameter sets collected from the SDP */
    uint8_t *sps, *pps, *sei;
    int sps_size, pps_size, sei_size;

    /* reassembly state for fragmentation units */
    AVIOContext *fu_buf;
    uint16_t expected_seq;
    int seq_valid;
};

/* decode the two-byte EVC NAL unit header, which doubles as the RFC 9584
 * payload header (RFC 9584, Section 1.1.4):
 *
 *     0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |F|   Type      | TID | Reserve|E|
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *       Forbidden zero (F): 1 bit
 *       nal_unit_type_plus1 (Type): 6 bits
 *       nuh_temporal_id (TID): 3 bits
 *       nuh_reserved_zero_5bits (Reserve): 5 bits
 *       nuh_extension_flag (E): 1 bit
 */
static inline int evc_payload_hdr_type(const uint8_t *buf)
{
    return (buf[0] >> 1) & 0x3f;
}

static inline int evc_payload_hdr_tid(const uint8_t *buf)
{
    return ((buf[0] & 0x01) << 2) | (buf[1] >> 6);
}

/* parse a comma-separated list of base64 encoded NAL units into a
 * length-prefixed NAL unit sequence, as used by all EVC elementary
 * stream handling in FFmpeg */
static int evc_parse_sprop_nal_units(AVFormatContext *s, uint8_t **data_ptr,
                                     int *size_ptr, const char *value)
{
    char base64_packet[1024];
    uint8_t decoded_packet[1024];

    while (*value) {
        char *dst = base64_packet;

        while (*value && *value != ',' &&
               (dst - base64_packet) < sizeof(base64_packet) - 1)
            *dst++ = *value++;
        *dst = '\0';
        if (*value == ',')
            value++;

        if (*base64_packet) {
            uint8_t *dest;
            int packet_size = av_base64_decode(decoded_packet, base64_packet,
                                               sizeof(decoded_packet));
            if (packet_size <= 0)
                continue;

            dest = av_realloc(*data_ptr, packet_size + *size_ptr +
                              EVC_NALU_LENGTH_PREFIX_SIZE);
            if (!dest) {
                av_log(s, AV_LOG_ERROR,
                       "Unable to allocate memory for EVC sprop parameters\n");
                return AVERROR(ENOMEM);
            }
            *data_ptr = dest;

            AV_WB32(dest + *size_ptr, packet_size);
            memcpy(dest + *size_ptr + EVC_NALU_LENGTH_PREFIX_SIZE,
                   decoded_packet, packet_size);
            *size_ptr += packet_size + EVC_NALU_LENGTH_PREFIX_SIZE;
        }
    }

    return 0;
}

static av_cold int evc_sdp_parse_fmtp_config(AVFormatContext *s,
                                             AVStream *stream,
                                             PayloadContext *evc_data,
                                             const char *attr, const char *value)
{
    /* profile-id: default 0 (Baseline profile) */
    if (!strcmp(attr, "profile-id")) {
        evc_data->profile_id = atoi(value);
        av_log(s, AV_LOG_TRACE, "SDP: found profile-id: %d\n",
               evc_data->profile_id);
    }

    /* level-id: 0-255, default 90 (level 3) */
    if (!strcmp(attr, "level-id")) {
        evc_data->level_id = atoi(value);
        av_log(s, AV_LOG_TRACE, "SDP: found level-id: %d\n",
               evc_data->level_id);
    }

    /* toolset-id: base64 encoded 64-bit mask of toolset_idc_h/_l
     * max-recv-level-id: 0-255 (receiver capability, not needed here) */

    /* sprop-sps: comma-separated list of [base64]
     * sprop-pps: comma-separated list of [base64]
     * sprop-sei: comma-separated list of [base64] */
    if (!strcmp(attr, "sprop-sps") || !strcmp(attr, "sprop-pps") ||
        !strcmp(attr, "sprop-sei")) {
        uint8_t **data_ptr;
        int *size_ptr;
        if (!strcmp(attr, "sprop-sps")) {
            data_ptr = &evc_data->sps;
            size_ptr = &evc_data->sps_size;
        } else if (!strcmp(attr, "sprop-pps")) {
            data_ptr = &evc_data->pps;
            size_ptr = &evc_data->pps_size;
        } else {
            data_ptr = &evc_data->sei;
            size_ptr = &evc_data->sei_size;
        }

        return evc_parse_sprop_nal_units(s, data_ptr, size_ptr, value);
    }

    /* sprop-max-don-diff: 0-32767
     *
     * When greater than 0, the DONL field is present in single NAL unit
     * packets, in the first aggregation unit of APs, and in FUs with the
     * S bit equal to 1 (RFC 9584, Sections 4.3.1-4.3.3). */
    if (!strcmp(attr, "sprop-max-don-diff")) {
        if (atoi(value) > 0)
            evc_data->using_donl_field = 1;
        av_log(s, AV_LOG_TRACE,
               "Found sprop-max-don-diff in SDP, DON field usage is: %d\n",
               evc_data->using_donl_field);
    }

    /* sprop-depack-buf-bytes: 0-4294967295
     * depack-buf-cap: 1-4294967295 */

    return 0;
}

static av_cold int evc_parse_sdp_line(AVFormatContext *ctx, int st_index,
                                      PayloadContext *evc_data, const char *line)
{
    AVStream *current_stream;
    AVCodecParameters *par;
    const char *sdp_line_ptr = line;

    if (st_index < 0)
        return 0;

    current_stream = ctx->streams[st_index];
    par = current_stream->codecpar;

    if (av_strstart(sdp_line_ptr, "framesize:", &sdp_line_ptr)) {
        ff_h264_parse_framesize(par, sdp_line_ptr);
    } else if (av_strstart(sdp_line_ptr, "fmtp:", &sdp_line_ptr)) {
        int ret = ff_parse_fmtp(ctx, current_stream, evc_data, sdp_line_ptr,
                                evc_sdp_parse_fmtp_config);
        if (evc_data->sps_size || evc_data->pps_size || evc_data->sei_size) {
            par->extradata_size = evc_data->sps_size + evc_data->pps_size +
                                  evc_data->sei_size;
            if ((ret = ff_alloc_extradata(par, par->extradata_size)) >= 0) {
                int pos = 0;
                memcpy(par->extradata + pos, evc_data->sps, evc_data->sps_size);
                pos += evc_data->sps_size;
                memcpy(par->extradata + pos, evc_data->pps, evc_data->pps_size);
                pos += evc_data->pps_size;
                memcpy(par->extradata + pos, evc_data->sei, evc_data->sei_size);
            }

            av_freep(&evc_data->sps);
            av_freep(&evc_data->pps);
            av_freep(&evc_data->sei);
            evc_data->sps_size = 0;
            evc_data->pps_size = 0;
            evc_data->sei_size = 0;
        }
        return ret;
    }

    return 0;
}

static void evc_reset_fu_buffer(PayloadContext *rtp_evc_ctx)
{
    uint8_t *dummy;

    if (rtp_evc_ctx->fu_buf) {
        avio_close_dyn_buf(rtp_evc_ctx->fu_buf, &dummy);
        av_free(dummy);
        rtp_evc_ctx->fu_buf = NULL;
    }
}

/* create an output packet holding a sequence of NAL units, each prefixed
 * with the four-byte length field used by the EVC elementary stream
 * format */
static int evc_output_nal_unit(AVPacket *pkt, const uint8_t *nal, int size)
{
    int res;

    if ((res = av_new_packet(pkt, EVC_NALU_LENGTH_PREFIX_SIZE + size)) < 0)
        return res;
    AV_WB32(pkt->data, size);
    memcpy(pkt->data + EVC_NALU_LENGTH_PREFIX_SIZE, nal, size);

    return 0;
}

static int evc_handle_aggregated_packet(AVFormatContext *ctx, AVPacket *pkt,
                                        const uint8_t *buf, int len)
{
    int pass, total_length = 0;
    uint8_t *dst = NULL;
    int res;

    /* two-pass processing: first determine the output size, then copy */
    for (pass = 0; pass < 2; pass++) {
        const uint8_t *src = buf;
        int src_len        = len;

        while (src_len > RTP_EVC_AP_NALU_LENGTH_FIELD_SIZE) {
            uint16_t nal_size = AV_RB16(src);

            src     += RTP_EVC_AP_NALU_LENGTH_FIELD_SIZE;
            src_len -= RTP_EVC_AP_NALU_LENGTH_FIELD_SIZE;

            if (nal_size > src_len) {
                av_log(ctx, AV_LOG_ERROR,
                       "RTP/EVC aggregation unit size exceeds length: %d %d\n",
                       nal_size, src_len);
                return AVERROR_INVALIDDATA;
            }

            if (pass == 0) {
                total_length += EVC_NALU_LENGTH_PREFIX_SIZE + nal_size;
            } else {
                AV_WB32(dst, nal_size);
                dst += EVC_NALU_LENGTH_PREFIX_SIZE;
                memcpy(dst, src, nal_size);
                dst += nal_size;
            }

            src     += nal_size;
            src_len -= nal_size;
        }

        if (pass == 0) {
            /* an AP must carry at least two aggregation units
             * (RFC 9584, Section 4.3.2) */
            if (total_length == 0)
                return AVERROR_INVALIDDATA;
            if ((res = av_new_packet(pkt, total_length)) < 0)
                return res;
            dst = pkt->data;
        }
    }

    return 0;
}

static int evc_handle_fu_packet(AVFormatContext *ctx,
                                PayloadContext *rtp_evc_ctx, AVPacket *pkt,
                                const uint8_t *rtp_pl, const uint8_t *buf,
                                int len, uint16_t seq)
{
    int first_fragment, last_fragment, fu_type;
    int res;

    /*
     *    decode the FU header (RFC 9584, Section 4.3.3)
     *
     *     0 1 2 3 4 5 6 7
     *    +-+-+-+-+-+-+-+-+
     *    |S|E|  FuType   |
     *    +---------------+
     *
     *       Start fragment (S): 1 bit
     *       End fragment (E): 1 bit
     *       FuType: 6 bits, equal to the Type field of the fragmented
     *               NAL unit
     */
    first_fragment = buf[0] & 0x80;
    last_fragment  = buf[0] & 0x40;
    fu_type        = buf[0] & 0x3f;

    buf += RTP_EVC_FU_HEADER_SIZE;
    len -= RTP_EVC_FU_HEADER_SIZE;

    /* pass the DONL field, present only in the first FU of a fragmented
     * NAL unit (RFC 9584, Section 4.3.3) */
    if (rtp_evc_ctx->using_donl_field && first_fragment) {
        buf += RTP_EVC_DONL_FIELD_SIZE;
        len -= RTP_EVC_DONL_FIELD_SIZE;
    }

    av_log(ctx, AV_LOG_TRACE, " FU type %d with %d bytes\n", fu_type, len);

    /* an FU payload MUST NOT be empty (RFC 9584, Section 4.3.3) */
    if (len <= 0) {
        if (len < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "Too short RTP/EVC packet, got %d bytes in FU\n", len);
            return AVERROR_INVALIDDATA;
        }
        return AVERROR(EAGAIN);
    }

    /* a non-fragmented NAL unit MUST NOT be transmitted in one FU
     * (RFC 9584, Section 4.3.3) */
    if (first_fragment && last_fragment) {
        av_log(ctx, AV_LOG_ERROR,
               "Illegal combination of S and E bit in RTP/EVC packet\n");
        return AVERROR_INVALIDDATA;
    }

    if (fu_type == 0 || fu_type > RTP_EVC_MAX_REAL_NAL_TYPE) {
        av_log(ctx, AV_LOG_ERROR, "Illegal RTP/EVC FuType %d\n", fu_type);
        return AVERROR_INVALIDDATA;
    }

    if (first_fragment) {
        uint8_t new_nal_header[EVC_NALU_HEADER_SIZE];

        /* discard an incomplete previous fragmented NAL unit, if any */
        evc_reset_fu_buffer(rtp_evc_ctx);

        if ((res = avio_open_dyn_buf(&rtp_evc_ctx->fu_buf)) < 0)
            return res;

        /* reconstruct the NAL unit header of the fragmented NAL unit
         * from the F, TID, Reserve and E fields of the payload header
         * and the FuType field of the FU header (RFC 9584, Section 6) */
        new_nal_header[0] = (rtp_pl[0] & 0x81) | (fu_type << 1);
        new_nal_header[1] = rtp_pl[1];
        avio_write(rtp_evc_ctx->fu_buf, new_nal_header,
                   sizeof(new_nal_header));
    } else {
        if (!rtp_evc_ctx->fu_buf) {
            /* the start of this fragmented NAL unit was lost; discard
             * all following fragments (RFC 9584, Section 4.3.3). The
             * loss itself was already reported. */
            av_log(ctx, AV_LOG_TRACE,
                   "Dropping RTP/EVC FU without start fragment\n");
            return AVERROR(EAGAIN);
        }
        if (rtp_evc_ctx->seq_valid &&
            seq != (uint16_t)(rtp_evc_ctx->expected_seq + 1)) {
            /* fragments of the same NAL unit must be sent in consecutive
             * order (RFC 9584, Section 4.3.3); drop the incomplete unit */
            av_log(ctx, AV_LOG_WARNING,
                   "Lost RTP packet within an EVC FU, dropping NAL unit\n");
            evc_reset_fu_buffer(rtp_evc_ctx);
            return AVERROR(EAGAIN);
        }
    }

    rtp_evc_ctx->expected_seq = seq;
    rtp_evc_ctx->seq_valid    = 1;

    avio_write(rtp_evc_ctx->fu_buf, buf, len);

    if (last_fragment) {
        uint8_t *fu_data;
        int fu_size = avio_close_dyn_buf(rtp_evc_ctx->fu_buf, &fu_data);

        rtp_evc_ctx->fu_buf = NULL;
        res = evc_output_nal_unit(pkt, fu_data, fu_size);
        av_free(fu_data);
        return res;
    }

    return AVERROR(EAGAIN);
}

static int evc_handle_packet(AVFormatContext *ctx, PayloadContext *rtp_evc_ctx,
                             AVStream *st, AVPacket *pkt, uint32_t *timestamp,
                             const uint8_t *buf, int len, uint16_t seq,
                             int flags)
{
    const uint8_t *rtp_pl = buf;
    int tid, type;
    int res = 0;

    /* sanity check for size of input packet: 1 byte payload at least */
    if (len < RTP_EVC_PAYLOAD_HEADER_SIZE + 1) {
        av_log(ctx, AV_LOG_ERROR, "Too short RTP/EVC packet, got %d bytes\n",
               len);
        return AVERROR_INVALIDDATA;
    }

    type = evc_payload_hdr_type(buf);
    tid  = evc_payload_hdr_tid(buf);

    /* a Type (nal_unit_type_plus1) value of 0 is illegal
     * (RFC 9584, Section 1.1.4) */
    if (!type) {
        av_log(ctx, AV_LOG_ERROR, "Illegal Type 0 in RTP/EVC packet\n");
        return AVERROR_INVALIDDATA;
    }

    av_log(ctx, AV_LOG_TRACE,
           "RTP/EVC packet: type %d, TID %d, %d bytes\n", type, tid, len);

    switch (type) {
    /* aggregation packet (AP) - with two or more NAL units */
    case RTP_EVC_AP_TYPE:
        /* pass the EVC payload header */
        buf += RTP_EVC_PAYLOAD_HEADER_SIZE;
        len -= RTP_EVC_PAYLOAD_HEADER_SIZE;

        /* pass the DONL field of the first aggregation unit; subsequent
         * aggregation units carry no DOND field in RFC 9584
         * (Section 4.3.2) */
        if (rtp_evc_ctx->using_donl_field) {
            buf += RTP_EVC_DONL_FIELD_SIZE;
            len -= RTP_EVC_DONL_FIELD_SIZE;
        }

        res = evc_handle_aggregated_packet(ctx, pkt, buf, len);
        if (res < 0)
            return res;
        break;
    /* fragmentation unit (FU) */
    case RTP_EVC_FU_TYPE:
        /* pass the EVC payload header */
        buf += RTP_EVC_PAYLOAD_HEADER_SIZE;
        len -= RTP_EVC_PAYLOAD_HEADER_SIZE;

        res = evc_handle_fu_packet(ctx, rtp_evc_ctx, pkt, rtp_pl, buf, len,
                                   seq);
        if (res < 0)
            return res;
        break;
    /* NAL-unit-like structures with Type values 58 to 62 are reserved
     * for future extensions and MUST NOT be passed to the decoder
     * (RFC 9584, Section 6) */
    case 58:
    case 59:
    case 60:
    case 61:
    case 62:
        avpriv_report_missing_feature(ctx, "RTP/EVC packet Type %d", type);
        res = AVERROR_PATCHWELCOME;
        break;
    /* single NAL unit packet (RFC 9584, Section 4.3.1) */
    default:
        if (rtp_evc_ctx->using_donl_field) {
            /* the conditional DONL field is located between the payload
             * header (which doubles as the NAL unit header) and the NAL
             * unit payload data and has to be removed */
            uint8_t nal_header[EVC_NALU_HEADER_SIZE];

            if (len < RTP_EVC_PAYLOAD_HEADER_SIZE + RTP_EVC_DONL_FIELD_SIZE + 1) {
                av_log(ctx, AV_LOG_ERROR,
                       "Too short RTP/EVC packet, got %d bytes\n", len);
                return AVERROR_INVALIDDATA;
            }

            memcpy(nal_header, buf, EVC_NALU_HEADER_SIZE);
            len -= RTP_EVC_DONL_FIELD_SIZE;
            if ((res = av_new_packet(pkt, EVC_NALU_LENGTH_PREFIX_SIZE + len)) < 0)
                return res;
            AV_WB32(pkt->data, len);
            memcpy(pkt->data + EVC_NALU_LENGTH_PREFIX_SIZE, nal_header,
                   EVC_NALU_HEADER_SIZE);
            memcpy(pkt->data + EVC_NALU_LENGTH_PREFIX_SIZE + EVC_NALU_HEADER_SIZE,
                   buf + RTP_EVC_PAYLOAD_HEADER_SIZE + RTP_EVC_DONL_FIELD_SIZE,
                   len - EVC_NALU_HEADER_SIZE);
        } else {
            res = evc_output_nal_unit(pkt, buf, len);
            if (res < 0)
                return res;
        }

        break;
    }

    pkt->stream_index = st->index;

    return res;
}

static av_cold void evc_close_context(PayloadContext *evc_data)
{
    evc_reset_fu_buffer(evc_data);
    av_freep(&evc_data->sps);
    av_freep(&evc_data->pps);
    av_freep(&evc_data->sei);
}

const RTPDynamicProtocolHandler ff_evc_dynamic_handler = {
    .enc_name         = "evc",
    .codec_type       = AVMEDIA_TYPE_VIDEO,
    .codec_id         = AV_CODEC_ID_EVC,
    .need_parsing     = AVSTREAM_PARSE_FULL,
    .priv_data_size   = sizeof(PayloadContext),
    .parse_sdp_a_line = evc_parse_sdp_line,
    .close            = evc_close_context,
    .parse_packet     = evc_handle_packet,
};
